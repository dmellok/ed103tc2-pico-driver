/*
 * Embed the landscape 1872x1404 16-grey sample frame into flash (.rodata, XIP)
 * via .incbin, so it8951_show_frame() can read it directly without a RAM copy.
 *
 * Regenerate src/sample_card.bin with  tools/gen_sample.py  (synthetic test
 * card) or replace it from your own image with  tools/png2frame.py .
 *
 * Path is relative to the build working directory (the project root).
 */
.section .rodata
.balign 4
.global sample_card
sample_card:
.incbin "src/sample_card.bin"
.global sample_card_end
sample_card_end:
