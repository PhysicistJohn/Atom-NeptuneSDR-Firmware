// SPDX-License-Identifier: MIT
// Hardware last line of defense. Authentication occurs in the PS, but every
// reset/fault/inhibit condition clears the PL arm latch and forces zero data.
module neptune_tx_safety_gate (
  input  logic               clk,
  input  logic               rst_n,
  input  logic               persistent_inhibit,
  input  logic               hard_disable,
  input  logic               thermal_fault,
  input  logic               watchdog_fault,
  input  logic               rf_pll_locked,
  input  logic               authenticated_arm_pulse,
  input  logic               authenticated_disarm_pulse,
  input  logic               sample_valid,
  input  logic signed [15:0] sample_i,
  input  logic signed [15:0] sample_q,
  output logic               tx_armed,
  output logic               tx_sample_valid,
  output logic signed [15:0] tx_sample_i,
  output logic signed [15:0] tx_sample_q,
  output logic [31:0]        disarm_revision,
  output logic [7:0]         last_disarm_reason
);
  localparam logic [7:0] REASON_RESET      = 8'h01;
  localparam logic [7:0] REASON_COMMAND    = 8'h02;
  localparam logic [7:0] REASON_INHIBIT    = 8'h03;
  localparam logic [7:0] REASON_HARD       = 8'h04;
  localparam logic [7:0] REASON_THERMAL    = 8'h05;
  localparam logic [7:0] REASON_WATCHDOG   = 8'h06;
  localparam logic [7:0] REASON_PLL_UNLOCK = 8'h07;

  wire safe_conditions = !persistent_inhibit && !hard_disable &&
                         !thermal_fault && !watchdog_fault && rf_pll_locked;
  wire effective_enable = tx_armed && safe_conditions;

  always_comb begin
    tx_sample_valid = sample_valid && effective_enable;
    tx_sample_i = effective_enable ? sample_i : 16'sd0;
    tx_sample_q = effective_enable ? sample_q : 16'sd0;
  end

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      tx_armed           <= 1'b0;
      disarm_revision    <= 32'd1;
      last_disarm_reason <= REASON_RESET;
    end else begin
      if (tx_armed && !safe_conditions) begin
        tx_armed        <= 1'b0;
        disarm_revision <= disarm_revision + 32'd1;
        if (hard_disable) last_disarm_reason <= REASON_HARD;
        else if (persistent_inhibit) last_disarm_reason <= REASON_INHIBIT;
        else if (thermal_fault) last_disarm_reason <= REASON_THERMAL;
        else if (watchdog_fault) last_disarm_reason <= REASON_WATCHDOG;
        else last_disarm_reason <= REASON_PLL_UNLOCK;
      end else if (authenticated_disarm_pulse) begin
        if (tx_armed)
          disarm_revision <= disarm_revision + 32'd1;
        tx_armed           <= 1'b0;
        last_disarm_reason <= REASON_COMMAND;
      end else if (authenticated_arm_pulse && safe_conditions) begin
        tx_armed <= 1'b1;
      end
    end
  end
endmodule
