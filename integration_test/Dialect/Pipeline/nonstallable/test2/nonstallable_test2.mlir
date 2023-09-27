// REQUIRES: iverilog,cocotb

// RUN: circt-opt %s -pipeline-explicit-regs -lower-pipeline-to-hw -lower-seq-to-sv -sv-trace-iverilog -export-verilog \
// RUN:     -o %t.mlir > %t.sv

// RUN: circt-cocotb-driver.py --objdir=%T --topLevel=nonstallable_test2 \
// RUN:     --pythonModule=nonstallable_test2 --pythonFolder="%S,%S/.." %t.sv 2>&1 | FileCheck %s


// CHECK: ** TEST
// CHECK: ** TESTS=[[N:.*]] PASS=[[N]] FAIL=0 SKIP=0

hw.module @nonstallable_test2(input %arg0: i32, input %go: i1, input %clock: !seq.clock, input %reset: i1, input %stall: i1, output out: i32, done : i1) {
  %out, %done = pipeline.scheduled "nonstallable_test2"(%a0 : i32 = %arg0)
      stall(%s = %stall) clock(%c = %clock) reset(%r = %reset) go(%g = %go)
        {stallability = [true, false, true, false, true]} -> (out : i32) {
    pipeline.stage ^bb1
  ^bb1(%s1_enable: i1):
    pipeline.stage ^bb2
  ^bb2(%s2_enable: i1):
    pipeline.stage ^bb3
  ^bb3(%s3_enable: i1):
    pipeline.stage ^bb4
  ^bb4(%s4_enable: i1):
    pipeline.stage ^bb5
  ^bb5(%s5_enable: i1):
    pipeline.return %a0 : i32
  }
  hw.output %out, %done : i32, i1
}
