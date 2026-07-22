// SPDX-License-Identifier: MIT
package neptune_crc32c_pkg;
  function automatic logic [31:0] crc32c_byte(
    input logic [31:0] crc,
    input logic [7:0] value
  );
    logic [31:0] next_crc;
    integer bit_index;
    begin
      next_crc = crc ^ value;
      for (bit_index = 0; bit_index < 8; bit_index = bit_index + 1)
        next_crc = next_crc[0] ?
                   ((next_crc >> 1) ^ 32'h82f63b78) :
                   (next_crc >> 1);
      crc32c_byte = next_crc;
    end
  endfunction
endpackage
