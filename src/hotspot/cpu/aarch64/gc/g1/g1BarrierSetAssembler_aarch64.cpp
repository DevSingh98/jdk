/*
 * Copyright (c) 2018, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "asm/macroAssembler.inline.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1BarrierSetAssembler.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/g1/g1CardTable.hpp"
#include "gc/g1/g1HeapRegion.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "interpreter/interp_masm.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/sharedRuntime.hpp"
#ifdef COMPILER1
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "gc/g1/c1/g1BarrierSetC1.hpp"
#endif // COMPILER1
#ifdef COMPILER2
#include "gc/g1/c2/g1BarrierSetC2.hpp"
#endif // COMPILER2

#define __ masm->

void G1BarrierSetAssembler::gen_write_ref_array_pre_barrier(MacroAssembler* masm, DecoratorSet decorators,
                                                            Register addr, Register count, RegSet saved_regs) {
  bool dest_uninitialized = (decorators & IS_DEST_UNINITIALIZED) != 0;
  if (!dest_uninitialized) {
    Label done;
    Address in_progress(rthread, in_bytes(G1ThreadLocalData::satb_mark_queue_active_offset()));

    // Is marking active?
    if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {
      __ ldrw(rscratch1, in_progress);
    } else {
      assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
      __ ldrb(rscratch1, in_progress);
    }
    __ cbzw(rscratch1, done);

    __ push(saved_regs, sp);
    if (count == c_rarg0) {
      if (addr == c_rarg1) {
        // exactly backwards!!
        __ mov(rscratch1, c_rarg0);
        __ mov(c_rarg0, c_rarg1);
        __ mov(c_rarg1, rscratch1);
      } else {
        __ mov(c_rarg1, count);
        __ mov(c_rarg0, addr);
      }
    } else {
      __ mov(c_rarg0, addr);
      __ mov(c_rarg1, count);
    }
    if (UseCompressedOops) {
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_array_pre_narrow_oop_entry), 2);
    } else {
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_array_pre_oop_entry), 2);
    }
    __ pop(saved_regs, sp);

    __ bind(done);
  }
}

void G1BarrierSetAssembler::gen_write_ref_array_post_barrier(MacroAssembler* masm, DecoratorSet decorators,
                                                             Register start, Register count, Register scratch, RegSet saved_regs) {
  __ push(saved_regs, sp);
  assert_different_registers(start, count, scratch);
  assert_different_registers(c_rarg0, count);
  __ mov(c_rarg0, start);
  __ mov(c_rarg1, count);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_array_post_entry), 2);
  __ pop(saved_regs, sp);
}

static void generate_queue_test_and_insertion(MacroAssembler* masm, ByteSize index_offset, ByteSize buffer_offset, Label& runtime,
                                              const Register thread, const Register value, const Register temp1, const Register temp2) {
  // Can we store a value in the given thread's buffer?
  // (The index field is typed as size_t.)
  __ ldr(temp1, Address(thread, in_bytes(index_offset)));   // temp1 := *(index address)
  __ cbz(temp1, runtime);                                   // jump to runtime if index == 0 (full buffer)
  // The buffer is not full, store value into it.
  __ sub(temp1, temp1, wordSize);                           // temp1 := next index
  __ str(temp1, Address(thread, in_bytes(index_offset)));   // *(index address) := next index
  __ ldr(temp2, Address(thread, in_bytes(buffer_offset)));  // temp2 := buffer address
  __ str(value, Address(temp2, temp1));                     // *(buffer address + next index) := value
}

static void generate_pre_barrier_fast_path(MacroAssembler* masm,
                                           const Register thread,
                                           const Register tmp1) {
  Address in_progress(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_active_offset()));
  // Is marking active?
  if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {
    __ ldrw(tmp1, in_progress);
  } else {
    assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
    __ ldrb(tmp1, in_progress);
  }
}

static void generate_pre_barrier_slow_path(MacroAssembler* masm,
                                           const Register obj,
                                           const Register pre_val,
                                           const Register thread,
                                           const Register tmp1,
                                           const Register tmp2,
                                           Label& done,
                                           Label& runtime) {
  // Do we need to load the previous value?
  if (obj != noreg) {
    __ load_heap_oop(pre_val, Address(obj, 0), noreg, noreg, AS_RAW);
  }
  // Is the previous value null?
  __ cbz(pre_val, done);
  generate_queue_test_and_insertion(masm,
                                    G1ThreadLocalData::satb_mark_queue_index_offset(),
                                    G1ThreadLocalData::satb_mark_queue_buffer_offset(),
                                    runtime,
                                    thread, pre_val, tmp1, tmp2);
  __ b(done);
}

void G1BarrierSetAssembler::g1_write_barrier_pre(MacroAssembler* masm,
                                                 Register obj,
                                                 Register pre_val,
                                                 Register thread,
                                                 Register tmp1,
                                                 Register tmp2,
                                                 bool tosca_live,
                                                 bool expand_call) {
  // If expand_call is true then we expand the call_VM_leaf macro
  // directly to skip generating the check by
  // InterpreterMacroAssembler::call_VM_leaf_base that checks _last_sp.

  assert(thread == rthread, "must be");

  Label done;
  Label runtime;

  assert_different_registers(obj, pre_val, tmp1, tmp2);
  assert(pre_val != noreg && tmp1 != noreg && tmp2 != noreg, "expecting a register");

  generate_pre_barrier_fast_path(masm, thread, tmp1);
  // If marking is not active (*(mark queue active address) == 0), jump to done
  __ cbzw(tmp1, done);
  generate_pre_barrier_slow_path(masm, obj, pre_val, thread, tmp1, tmp2, done, runtime);

  __ bind(runtime);

  __ push_call_clobbered_registers();

  // Calling the runtime using the regular call_VM_leaf mechanism generates
  // code (generated by InterpreterMacroAssember::call_VM_leaf_base)
  // that checks that the *(rfp+frame::interpreter_frame_last_sp) == nullptr.
  //
  // If we care generating the pre-barrier without a frame (e.g. in the
  // intrinsified Reference.get() routine) then rfp might be pointing to
  // the caller frame and so this check will most likely fail at runtime.
  //
  // Expanding the call directly bypasses the generation of the check.
  // So when we do not have have a full interpreter frame on the stack
  // expand_call should be passed true.

  if (expand_call) {
    assert(pre_val != c_rarg1, "smashed arg");
    __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry), pre_val, thread);
  } else {
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry), pre_val, thread);
  }

  __ pop_call_clobbered_registers();

  __ bind(done);

}

static void generate_post_barrier_fast_path(MacroAssembler* masm,
                                            const Register store_addr,
                                            const Register new_val,
                                            const Register tmp1,
                                            const Register tmp2,
                                            Label& done,
                                            bool new_val_may_be_null) {
  // Does store cross heap regions?
  __ eor(tmp1, store_addr, new_val);                     // tmp1 := store address ^ new value
  __ lsr(tmp1, tmp1, G1HeapRegion::LogOfHRGrainBytes);   // tmp1 := ((store address ^ new value) >> LogOfHRGrainBytes)
  __ cbz(tmp1, done);
  // Crosses regions, storing null?
  if (new_val_may_be_null) {
    __ cbz(new_val, done);
  }
  // Storing region crossing non-null, is card young?
  __ lsr(tmp1, store_addr, CardTable::card_shift());     // tmp1 := card address relative to card table base
  __ load_byte_map_base(tmp2);                           // tmp2 := card table base address
  __ add(tmp1, tmp1, tmp2);                              // tmp1 := card address
  __ ldrb(tmp2, Address(tmp1));                          // tmp2 := card
  __ cmpw(tmp2, (int)G1CardTable::g1_young_card_val());  // tmp2 := card == young_card_val?
}

static void generate_post_barrier_slow_path(MacroAssembler* masm,
                                            const Register thread,
                                            const Register tmp1,
                                            const Register tmp2,
                                            Label& done,
                                            Label& runtime) {
  __ membar(Assembler::StoreLoad);  // StoreLoad membar
  __ ldrb(tmp2, Address(tmp1));     // tmp2 := card
  __ cbzw(tmp2, done);
  // Storing a region crossing, non-null oop, card is clean.
  // Dirty card and log.
  STATIC_ASSERT(CardTable::dirty_card_val() == 0);
  __ strb(zr, Address(tmp1));       // *(card address) := dirty_card_val
  generate_queue_test_and_insertion(masm,
                                    G1ThreadLocalData::dirty_card_queue_index_offset(),
                                    G1ThreadLocalData::dirty_card_queue_buffer_offset(),
                                    runtime,
                                    thread, tmp1, tmp2, rscratch1);
  __ b(done);
}

void G1BarrierSetAssembler::g1_write_barrier_post(MacroAssembler* masm,
                                                  Register store_addr,
                                                  Register new_val,
                                                  Register thread,
                                                  Register tmp1,
                                                  Register tmp2) {
  assert(thread == rthread, "must be");
  assert_different_registers(store_addr, new_val, thread, tmp1, tmp2,
                             rscratch1);
  assert(store_addr != noreg && new_val != noreg && tmp1 != noreg
         && tmp2 != noreg, "expecting a register");

  Label done;
  Label runtime;

  generate_post_barrier_fast_path(masm, store_addr, new_val, tmp1, tmp2, done, true /* new_val_may_be_null */);
  // If card is young, jump to done
  __ br(Assembler::EQ, done);
  generate_post_barrier_slow_path(masm, thread, tmp1, tmp2, done, runtime);

  __ bind(runtime);
  // save the live input values
  RegSet saved = RegSet::of(store_addr);
  __ push(saved, sp);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_post_entry), tmp1, thread);
  __ pop(saved, sp);

  __ bind(done);
}

#if defined(COMPILER2)

static void generate_c2_barrier_runtime_call(MacroAssembler* masm, G1BarrierStubC2* stub, const Register arg, const address runtime_path) {
  SaveLiveRegisters save_registers(masm, stub);
  if (c_rarg0 != arg) {
    __ mov(c_rarg0, arg);
  }
  __ mov(c_rarg1, rthread);
  __ mov(rscratch1, runtime_path);
  __ blr(rscratch1);
}

void G1BarrierSetAssembler::g1_write_barrier_pre_c2(MacroAssembler* masm,
                                                    Register obj,
                                                    Register pre_val,
                                                    Register thread,
                                                    Register tmp1,
                                                    Register tmp2,
                                                    G1PreBarrierStubC2* stub) {
  assert(thread == rthread, "must be");
  assert_different_registers(obj, pre_val, tmp1, tmp2);
  assert(pre_val != noreg && tmp1 != noreg && tmp2 != noreg, "expecting a register");

  stub->initialize_registers(obj, pre_val, thread, tmp1, tmp2);

  generate_pre_barrier_fast_path(masm, thread, tmp1);
  // If marking is active (*(mark queue active address) != 0), jump to stub (slow path)
  __ cbnzw(tmp1, *stub->entry());

  __ bind(*stub->continuation());
}

void G1BarrierSetAssembler::generate_c2_pre_barrier_stub(MacroAssembler* masm,
                                                         G1PreBarrierStubC2* stub) const {
  Assembler::InlineSkippedInstructionsCounter skip_counter(masm);
  Label runtime;
  Register obj = stub->obj();
  Register pre_val = stub->pre_val();
  Register thread = stub->thread();
  Register tmp1 = stub->tmp1();
  Register tmp2 = stub->tmp2();

  __ bind(*stub->entry());
  generate_pre_barrier_slow_path(masm, obj, pre_val, thread, tmp1, tmp2, *stub->continuation(), runtime);

  __ bind(runtime);
  generate_c2_barrier_runtime_call(masm, stub, pre_val, CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry));
  __ b(*stub->continuation());
}

void G1BarrierSetAssembler::g1_write_barrier_post_c2(MacroAssembler* masm,
                                                     Register store_addr,
                                                     Register new_val,
                                                     Register thread,
                                                     Register tmp1,
                                                     Register tmp2,
                                                     G1PostBarrierStubC2* stub) {
  assert(thread == rthread, "must be");
  assert_different_registers(store_addr, new_val, thread, tmp1, tmp2,
                             rscratch1);
  assert(store_addr != noreg && new_val != noreg && tmp1 != noreg
         && tmp2 != noreg, "expecting a register");

  stub->initialize_registers(thread, tmp1, tmp2);

  bool new_val_may_be_null = (stub->barrier_data() & G1C2BarrierPostNotNull) == 0;
  generate_post_barrier_fast_path(masm, store_addr, new_val, tmp1, tmp2, *stub->continuation(), new_val_may_be_null);
  // If card is not young, jump to stub (slow path)
  __ br(Assembler::NE, *stub->entry());

  __ bind(*stub->continuation());
}

void G1BarrierSetAssembler::generate_c2_post_barrier_stub(MacroAssembler* masm,
                                                          G1PostBarrierStubC2* stub) const {
  Assembler::InlineSkippedInstructionsCounter skip_counter(masm);
  Label runtime;
  Register thread = stub->thread();
  Register tmp1 = stub->tmp1(); // tmp1 holds the card address.
  Register tmp2 = stub->tmp2();
  assert(stub->tmp3() == noreg, "not needed in this platform");

  __ bind(*stub->entry());
  generate_post_barrier_slow_path(masm, thread, tmp1, tmp2, *stub->continuation(), runtime);

  __ bind(runtime);
  generate_c2_barrier_runtime_call(masm, stub, tmp1, CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_post_entry));
  __ b(*stub->continuation());
}

#endif // COMPILER2

void G1BarrierSetAssembler::load_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                    Register dst, Address src, Register tmp1, Register tmp2) {
  bool on_oop = is_reference_type(type);
  bool on_weak = (decorators & ON_WEAK_OOP_REF) != 0;
  bool on_phantom = (decorators & ON_PHANTOM_OOP_REF) != 0;
  bool on_reference = on_weak || on_phantom;
  ModRefBarrierSetAssembler::load_at(masm, decorators, type, dst, src, tmp1, tmp2);
  if (on_oop && on_reference) {
    // LR is live.  It must be saved around calls.
    __ enter(/*strip_ret_addr*/true); // barrier may call runtime
    // Generate the G1 pre-barrier code to log the value of
    // the referent field in an SATB buffer.
    g1_write_barrier_pre(masm /* masm */,
                         noreg /* obj */,
                         dst /* pre_val */,
                         rthread /* thread */,
                         tmp1 /* tmp1 */,
                         tmp2 /* tmp2 */,
                         true /* tosca_live */,
                         true /* expand_call */);
    __ leave();
  }
}

void G1BarrierSetAssembler::oop_store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                         Address dst, Register val, Register tmp1, Register tmp2, Register tmp3) {
  // flatten object address if needed
  if (dst.index() == noreg && dst.offset() == 0) {
    if (dst.base() != tmp3) {
      __ mov(tmp3, dst.base());
    }
  } else {
    __ lea(tmp3, dst);
  }

  g1_write_barrier_pre(masm,
                       tmp3 /* obj */,
                       tmp2 /* pre_val */,
                       rthread /* thread */,
                       tmp1  /* tmp1 */,
                       rscratch2  /* tmp2 */,
                       val != noreg /* tosca_live */,
                       false /* expand_call */);

  if (val == noreg) {
    BarrierSetAssembler::store_at(masm, decorators, type, Address(tmp3, 0), noreg, noreg, noreg, noreg);
  } else {
    // G1 barrier needs uncompressed oop for region cross check.
    Register new_val = val;
    if (UseCompressedOops) {
      new_val = rscratch2;
      __ mov(new_val, val);
    }
    BarrierSetAssembler::store_at(masm, decorators, type, Address(tmp3, 0), val, noreg, noreg, noreg);
    g1_write_barrier_post(masm,
                          tmp3 /* store_adr */,
                          new_val /* new_val */,
                          rthread /* thread */,
                          tmp1 /* tmp1 */,
                          tmp2 /* tmp2 */);
  }

}

#ifdef COMPILER1

#undef __
#define __ ce->masm()->

void G1BarrierSetAssembler::gen_pre_barrier_stub(LIR_Assembler* ce, G1PreBarrierStub* stub) {
  G1BarrierSetC1* bs = (G1BarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
  // At this point we know that marking is in progress.
  // If do_load() is true then we have to emit the
  // load of the previous value; otherwise it has already
  // been loaded into _pre_val.

  __ bind(*stub->entry());

  assert(stub->pre_val()->is_register(), "Precondition.");

  Register pre_val_reg = stub->pre_val()->as_register();

  if (stub->do_load()) {
    ce->mem2reg(stub->addr(), stub->pre_val(), T_OBJECT, stub->patch_code(), stub->info(), false /*wide*/);
  }
  __ cbz(pre_val_reg, *stub->continuation());
  ce->store_parameter(stub->pre_val()->as_register(), 0);
  __ far_call(RuntimeAddress(bs->pre_barrier_c1_runtime_code_blob()->code_begin()));
  __ b(*stub->continuation());
}

void G1BarrierSetAssembler::gen_post_barrier_stub(LIR_Assembler* ce, G1PostBarrierStub* stub) {
  G1BarrierSetC1* bs = (G1BarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
  __ bind(*stub->entry());
  assert(stub->addr()->is_register(), "Precondition.");
  assert(stub->new_val()->is_register(), "Precondition.");
  Register new_val_reg = stub->new_val()->as_register();
  __ cbz(new_val_reg, *stub->continuation());
  ce->store_parameter(stub->addr()->as_pointer_register(), 0);
  __ far_call(RuntimeAddress(bs->post_barrier_c1_runtime_code_blob()->code_begin()));
  __ b(*stub->continuation());
}

#undef __

#define __ sasm->

void G1BarrierSetAssembler::generate_c1_pre_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("g1_pre_barrier", false);

  // arg0 : previous value of memory

  BarrierSet* bs = BarrierSet::barrier_set();

  const Register pre_val = r0;
  const Register thread = rthread;
  const Register tmp = rscratch1;

  Address in_progress(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_active_offset()));
  Address queue_index(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_index_offset()));
  Address buffer(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_buffer_offset()));

  Label done;
  Label runtime;

  // Is marking still active?
  if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {
    __ ldrw(tmp, in_progress);
  } else {
    assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
    __ ldrb(tmp, in_progress);
  }
  __ cbzw(tmp, done);

  // Can we store original value in the thread's buffer?
  __ ldr(tmp, queue_index);
  __ cbz(tmp, runtime);

  __ sub(tmp, tmp, wordSize);
  __ str(tmp, queue_index);
  __ ldr(rscratch2, buffer);
  __ add(tmp, tmp, rscratch2);
  __ load_parameter(0, rscratch2);
  __ str(rscratch2, Address(tmp, 0));
  __ b(done);

  __ bind(runtime);
  __ push_call_clobbered_registers();
  __ load_parameter(0, pre_val);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry), pre_val, thread);
  __ pop_call_clobbered_registers();
  __ bind(done);

  __ epilogue();
}

void G1BarrierSetAssembler::generate_c1_post_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("g1_post_barrier", false);

  // arg0: store_address
  Address store_addr(rfp, 2*BytesPerWord);

  BarrierSet* bs = BarrierSet::barrier_set();
  CardTableBarrierSet* ctbs = barrier_set_cast<CardTableBarrierSet>(bs);
  CardTable* ct = ctbs->card_table();

  Label done;
  Label runtime;

  // At this point we know new_value is non-null and the new_value crosses regions.
  // Must check to see if card is already dirty

  const Register thread = rthread;

  Address queue_index(thread, in_bytes(G1ThreadLocalData::dirty_card_queue_index_offset()));
  Address buffer(thread, in_bytes(G1ThreadLocalData::dirty_card_queue_buffer_offset()));

  const Register card_offset = rscratch2;
  // LR is free here, so we can use it to hold the byte_map_base.
  const Register byte_map_base = lr;

  assert_different_registers(card_offset, byte_map_base, rscratch1);

  __ load_parameter(0, card_offset);
  __ lsr(card_offset, card_offset, CardTable::card_shift());
  __ load_byte_map_base(byte_map_base);
  __ ldrb(rscratch1, Address(byte_map_base, card_offset));
  __ cmpw(rscratch1, (int)G1CardTable::g1_young_card_val());
  __ br(Assembler::EQ, done);

  assert((int)CardTable::dirty_card_val() == 0, "must be 0");

  __ membar(Assembler::StoreLoad);
  __ ldrb(rscratch1, Address(byte_map_base, card_offset));
  __ cbzw(rscratch1, done);

  // storing region crossing non-null, card is clean.
  // dirty card and log.
  __ strb(zr, Address(byte_map_base, card_offset));

  // Convert card offset into an address in card_addr
  Register card_addr = card_offset;
  __ add(card_addr, byte_map_base, card_addr);

  __ ldr(rscratch1, queue_index);
  __ cbz(rscratch1, runtime);
  __ sub(rscratch1, rscratch1, wordSize);
  __ str(rscratch1, queue_index);

  // Reuse LR to hold buffer_addr
  const Register buffer_addr = lr;

  __ ldr(buffer_addr, buffer);
  __ str(card_addr, Address(buffer_addr, rscratch1));
  __ b(done);

  __ bind(runtime);
  __ push_call_clobbered_registers();
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_post_entry), card_addr, thread);
  __ pop_call_clobbered_registers();
  __ bind(done);
  __ epilogue();
}

#undef __

#endif // COMPILER1
