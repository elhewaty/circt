// RUN: circt-opt %s -verify-diagnostics | circt-opt -verify-diagnostics | FileCheck %s

// CHECK-LABEL:  hw.module @unscheduled1(input %arg0 : i32, input %arg1 : i32, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out : i32) {
// CHECK-NEXT:    %dataOutputs, %done = pipeline.unscheduled(%arg0_0 : i32 = %arg0, %arg1_1 : i32 = %arg1) clock(%arg2 = %clk) reset(%arg3 = %rst) go(%arg4 = %go) -> (out : i32) {
// CHECK-NEXT:      %0 = pipeline.latency 2 -> (i32) {
// CHECK-NEXT:        %1 = comb.add %arg0_0, %arg1_1 : i32
// CHECK-NEXT:        pipeline.latency.return %1 : i32
// CHECK-NEXT:      }
// CHECK-NEXT:      pipeline.return %0 : i32
// CHECK-NEXT:    }
// CHECK-NEXT:    hw.output %dataOutputs : i32
// CHECK-NEXT:  }
hw.module @unscheduled1(input %arg0 : i32, input %arg1 : i32, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out: i32) {
  %0:2 = pipeline.unscheduled(%a0 : i32 = %arg0, %a1 : i32 = %arg1) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out: i32){
    %0 = pipeline.latency 2 -> (i32) {
      %1 = comb.add %a0, %a1 : i32
      pipeline.latency.return %1 : i32
    }
    pipeline.return %0 : i32
  }
  hw.output %0 : i32
}

// CHECK-LABEL:  hw.module @scheduled1(input %arg0 : i32, input %arg1 : i32, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out : i32) {
// CHECK-NEXT:    %out, %done = pipeline.scheduled(%a0 : i32 = %arg0, %a1 : i32 = %arg1) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out : i32) {
// CHECK-NEXT:      %0 = comb.add %a0, %a1 : i32
// CHECK-NEXT:      pipeline.stage ^bb1
// CHECK-NEXT:    ^bb1(%s1_enable: i1):  // pred: ^bb0
// CHECK-NEXT:      pipeline.return %0 : i32
// CHECK-NEXT:    }
// CHECK-NEXT:    hw.output %out : i32
// CHECK-NEXT:  }
hw.module @scheduled1(input %arg0 : i32, input %arg1 : i32, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out: i32) {
  %0:2 = pipeline.scheduled(%a0 : i32 = %arg0, %a1 : i32 = %arg1) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out: i32){
    %0 = comb.add %a0, %a1 : i32
    pipeline.stage ^bb1

   ^bb1(%s1_enable : i1):
    pipeline.return %0 : i32
  }
  hw.output %0 : i32
}


// CHECK-LABEL:  hw.module @scheduled2(input %arg0 : i32, input %arg1 : i32, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out : i32) {
// CHECK-NEXT:    %out, %done = pipeline.scheduled(%a0 : i32 = %arg0, %a1 : i32 = %arg1) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out : i32) {
// CHECK-NEXT:      %0 = comb.add %a0, %a1 : i32
// CHECK-NEXT:      pipeline.stage ^bb1 regs(%0 : i32)
// CHECK-NEXT:    ^bb1(%s1_reg0: i32, %s1_enable: i1):  // pred: ^bb0
// CHECK-NEXT:      pipeline.return %s1_reg0 : i32
// CHECK-NEXT:    }
// CHECK-NEXT:    hw.output %out : i32
// CHECK-NEXT:  }
hw.module @scheduled2(input %arg0 : i32, input %arg1 : i32, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out: i32) {
  %0:2 = pipeline.scheduled(%a0 : i32 = %arg0, %a1 : i32 = %arg1) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out: i32) {
    %0 = comb.add %a0, %a1 : i32
    pipeline.stage ^bb1 regs(%0 : i32)

   ^bb1(%s0_0 : i32, %s1_enable : i1):
    pipeline.return %s0_0 : i32
  }
  hw.output %0 : i32
}

// CHECK-LABEL:  hw.module @scheduledWithPassthrough(input %arg0 : i32, input %arg1 : i32, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out : i32) {
// CHECK-NEXT:    %out0, %out1, %done = pipeline.scheduled(%a0 : i32 = %arg0, %a1 : i32 = %arg1) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out0 : i32, out1 : i32) {
// CHECK-NEXT:      %0 = comb.add %a0, %a1 : i32
// CHECK-NEXT:      pipeline.stage ^bb1 regs(%0 : i32) pass(%a1 : i32)
// CHECK-NEXT:    ^bb1(%s1_reg0: i32, %s1_pass0: i32, %s1_enable: i1):  // pred: ^bb0
// CHECK-NEXT:      pipeline.return %s1_reg0, %s1_pass0 : i32, i32
// CHECK-NEXT:    }
// CHECK-NEXT:    hw.output %out0 : i32
// CHECK-NEXT:  }
hw.module @scheduledWithPassthrough(input %arg0 : i32, input %arg1 : i32, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out: i32) {
  %0:3 = pipeline.scheduled(%a0 : i32 = %arg0, %a1 : i32 = %arg1) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out0: i32, out1: i32) {
    %0 = comb.add %a0, %a1 : i32
    pipeline.stage ^bb1 regs(%0 : i32) pass(%a1 : i32)

   ^bb1(%s0_0 : i32, %s0_pass_a1 : i32, %s1_enable : i1):
    pipeline.return %s0_0, %s0_pass_a1 : i32, i32
  }
  hw.output %0#0 : i32
}

// CHECK-LABEL:  hw.module @withStall(input %arg0 : i32, input %stall : i1, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out : i32) {
// CHECK-NEXT:    %out, %done = pipeline.scheduled(%a0 : i32 = %arg0) stall(%s = %stall) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out : i32) {
// CHECK-NEXT:      pipeline.return %a0 : i32
// CHECK-NEXT:    }
// CHECK-NEXT:    hw.output %out : i32
// CHECK-NEXT:  }
hw.module @withStall(input %arg0 : i32, input %stall : i1, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out: i32) {
  %0:2 = pipeline.scheduled(%a0 : i32 = %arg0) stall(%s = %stall) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out: i32) {
    pipeline.return %a0 : i32
  }
  hw.output %0 : i32
}

// CHECK-LABEL:  hw.module @withMultipleRegs(input %arg0 : i32, input %stall : i1, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out : i32) {
// CHECK-NEXT:    %out, %done = pipeline.scheduled(%a0 : i32 = %arg0) stall(%s = %stall) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out : i32) {
// CHECK-NEXT:      pipeline.stage ^bb1 regs(%a0 : i32, %a0 : i32)
// CHECK-NEXT:    ^bb1(%s1_reg0: i32, %s1_reg1: i32, %s1_enable: i1):  // pred: ^bb0
// CHECK-NEXT:      pipeline.return %s1_reg0 : i32
// CHECK-NEXT:    }
// CHECK-NEXT:    hw.output %out : i32
// CHECK-NEXT:  }
hw.module @withMultipleRegs(input %arg0 : i32, input %stall : i1, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out: i32) {
  %0:2 = pipeline.scheduled(%a0 : i32 = %arg0) stall(%s = %stall) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out: i32) {
    pipeline.stage ^bb1 regs(%a0 : i32, %a0 : i32)

   ^bb1(%0 : i32, %1 : i32, %s1_enable : i1):
    pipeline.return %0 : i32
  }
  hw.output %0 : i32
}

// CHECK-LABEL:  hw.module @withClockGates(input %arg0 : i32, input %stall : i1, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out : i32) {
// CHECK-NEXT:    %out, %done = pipeline.scheduled(%a0 : i32 = %arg0) stall(%s = %stall) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out : i32) {
// CHECK-NEXT:      %true = hw.constant true
// CHECK-NEXT:      %true_0 = hw.constant true
// CHECK-NEXT:      %true_1 = hw.constant true
// CHECK-NEXT:      pipeline.stage ^bb1 regs(%a0 : i32 gated by [%true], %a0 : i32, %a0 : i32 gated by [%true_0, %true_1])
// CHECK-NEXT:    ^bb1(%s1_reg0: i32, %s1_reg1: i32, %s1_reg2: i32, %s1_enable: i1):  // pred: ^bb0
// CHECK-NEXT:      pipeline.return %s1_reg0 : i32
// CHECK-NEXT:    }
// CHECK-NEXT:    hw.output %out : i32
// CHECK-NEXT:  }
hw.module @withClockGates(input %arg0 : i32, input %stall : i1, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out: i32) {
  %0:2 = pipeline.scheduled(%a0 : i32 = %arg0) stall(%s = %stall) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out: i32) {
    %true1 = hw.constant true
    %true2 = hw.constant true
    %true3 = hw.constant true
    pipeline.stage ^bb1 regs(%a0 : i32 gated by [%true1], %a0 : i32, %a0 : i32 gated by [%true2, %true3])

   ^bb1(%0 : i32, %1 : i32, %2 : i32, %s1_enable : i1):
    pipeline.return %0 : i32
  }
  hw.output %0 : i32
}

// CHECK-LABEL:  hw.module @withNames(input %arg0 : i32, input %arg1 : i32, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out : i32) {
// CHECK-NEXT:    %out, %done = pipeline.scheduled "MyPipeline"(%a0 : i32 = %arg0, %a1 : i32 = %arg1) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out : i32) {
// CHECK-NEXT:      %0 = comb.add %a0, %a1 : i32
// CHECK-NEXT:      pipeline.stage ^bb1 regs("myAdd" = %0 : i32, %0 : i32, "myOtherAdd" = %0 : i32)
// CHECK-NEXT:    ^bb1(%myAdd: i32, %s1_reg1: i32, %myOtherAdd: i32, %s1_enable: i1):  // pred: ^bb0
// CHECK-NEXT:      pipeline.return %myAdd : i32
// CHECK-NEXT:    }
// CHECK-NEXT:    hw.output %out : i32
// CHECK-NEXT:  }
hw.module @withNames(input %arg0 : i32, input %arg1 : i32, input %go : i1, input %clk : !seq.clock, input %rst : i1, output %out: i32) {
  %0:2 = pipeline.scheduled "MyPipeline"(%a0 : i32 = %arg0, %a1 : i32 = %arg1) clock(%c = %clk) reset(%r = %rst) go(%g = %go) -> (out: i32){
    %0 = comb.add %a0, %a1 : i32
    pipeline.stage ^bb1 regs("myAdd" = %0 : i32, %0 : i32, "myOtherAdd" = %0 : i32)

   ^bb1(%r1 : i32, %r2 : i32, %r3 : i32, %s1_enable : i1):
    pipeline.return %r1 : i32
  }
  hw.output %0 : i32
}

// CHECK-LABEL:   hw.module @withStallability(
// CHECK:           %out, %done = pipeline.scheduled "MyPipeline"(%a0 : i32 = %arg0) stall(%s = %stall) clock(%c = %clk) reset(%r = %rst) go(%g = %go) {stallability = [true, false, true]} -> (out : i32)
hw.module @withStallability(input %arg0 : i32, input %go : i1, input %clk : !seq.clock, input %rst : i1, input %stall : i1, output %out: i32) {
  %0:2 = pipeline.scheduled "MyPipeline"(%a0 : i32 = %arg0) stall(%s = %stall) clock(%c = %clk) reset(%r = %rst) go(%g = %go)
    {stallability = [true, false, true]}
   -> (out: i32) {
    pipeline.stage ^bb1
   ^bb1(%s1_enable : i1):
    pipeline.stage ^bb2
   ^bb2(%s2_enable : i1):
    pipeline.stage ^bb3
   ^bb3(%s3_enable : i1):
    pipeline.return %a0 : i32
  }
  hw.output %0 : i32
}
