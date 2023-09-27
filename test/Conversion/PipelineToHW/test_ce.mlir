// RUN: circt-opt --lower-pipeline-to-hw="clock-gate-regs" %s | FileCheck %s


// CHECK:   hw.module @testSingle(input %[[VAL_0:.*]] : i32, input %[[VAL_1:.*]] : i32, input %[[VAL_2:.*]] : i1, input %[[CLOCK:.*]] : !seq.clock, input %[[VAL_4:.*]] : i1, output out0 : i32, output out1 : i1) {
// CHECK:           %[[VAL_5:.*]] = comb.sub %[[VAL_0]], %[[VAL_1]] : i32
// CHECK:           %[[VAL_6:.*]] = seq.clock_gate %[[CLOCK]], %[[VAL_2]]
// CHECK:           %[[VAL_7:.*]] = seq.compreg sym @p0_stage0_reg0 %[[VAL_5]], %[[VAL_6]] : i32
// CHECK:           %[[VAL_8:.*]] = seq.compreg sym @p0_stage0_reg1 %[[VAL_0]], %[[VAL_6]] : i32
// CHECK:           %[[VAL_9:.*]] = hw.constant false
// CHECK:           %[[VAL_10:.*]] = seq.compreg sym @p0_stage1_enable %[[VAL_2]], %[[CLOCK]], %[[VAL_4]], %[[VAL_9]]  : i1
// CHECK:           %[[VAL_11:.*]] = comb.add %[[VAL_7]], %[[VAL_8]] : i32
// CHECK:           hw.output %[[VAL_11]], %[[VAL_10]] : i32, i1
// CHECK:         }

hw.module @testSingle(input %arg0: i32, input %arg1: i32, input %go: i1, input %clk: !seq.clock, input %rst: i1, output out0: i32, output out1: i1) {
  %0:2 = pipeline.scheduled(%a0 : i32 = %arg0, %a1 : i32 = %arg1) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out: i32){
    %1 = comb.sub %a0,%a1 : i32
    pipeline.stage ^bb1 regs(%1 : i32, %a0 : i32)
  ^bb1(%6: i32, %7: i32, %s1_enable : i1):  // pred: ^bb1
    %8 = comb.add %6, %7 : i32
    pipeline.return %8 : i32
  }
  hw.output %0#0, %0#1 : i32, i1
}
