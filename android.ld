PHDRS
{
  phdr PT_PHDR PHDRS;
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
  dynamic PT_DYNAMIC;
}

SECTIONS
{
  . = 0;
  .text : { *(.text*) } :text
  .rodata : { *(.rodata*) } :text
  .data : { *(.data*) } :data
  .dynamic : { *(.dynamic) } :data :dynamic
  .got : { *(.got*) } :data
  .bss : { *(.bss*) } :data
}
