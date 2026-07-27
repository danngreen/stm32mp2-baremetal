/* Minimal Cortex-M33 startup for the STM32MP257F coprocessor.
 *
 * The A35 loads this image into SRAM2, points CA35SYSCFG->M33_INITSVTOR_CR at
 * the vector table (the M33 runs secure), and releases CPU2 from hold-boot. On
 * reset the M33 loads SP from vector[0] and PC from vector[1] (Reset_Handler);
 * because INITSVTOR is the secure VTOR, no VTOR write is needed at runtime.
 *
 * Unlike the copro_m33_embedded version, the vector table here is extended past
 * the 16 core exceptions to cover the external interrupts, because this demo
 * takes IPCC1_RX_S (IRQ 173 in the M33's NVIC numbering -- note it differs from
 * the A35's GIC number for the same event).
 */

	.syntax unified
	.cpu cortex-m33
	.fpu fpv5-sp-d16
	.thumb

.global g_pfnVectors
.global Default_Handler

.word _sidata
.word _sdata
.word _edata
.word _sbss
.word _ebss

	.section .text.Reset_Handler,"ax",%progbits
	.weak Reset_Handler
	.type Reset_Handler, %function
Reset_Handler:
	ldr   sp, =_estack

	/* Boot breadcrumb for the A35: prove the M33 executed its first
	 * instructions by writing a magic word into an unused vector slot
	 * (slot 8, offset 0x20). A second magic goes into slot 9 just before
	 * main(). Slots 8-10 are Reserved in ARMv8-M, so this is harmless. */
	ldr   r0, =g_pfnVectors
	ldr   r1, =0xB007C0DE
	str   r1, [r0, #0x20]
	dsb

	/* Enable CP10/CP11 (FPU) full access: SCB->CPACR |= (0xF << 20) */
	ldr   r0, =0xE000ED88
	ldr   r1, [r0]
	orr   r1, r1, #(0xF << 20)
	str   r1, [r0]
	dsb
	isb

	/* Copy .data from LMA to VMA (a no-op here since LMA == VMA). */
	movs  r1, #0
	b     LoopCopyDataInit

CopyDataInit:
	ldr   r3, =_sidata
	ldr   r3, [r3, r1]
	str   r3, [r0, r1]
	adds  r1, r1, #4

LoopCopyDataInit:
	ldr   r0, =_sdata
	ldr   r3, =_edata
	adds  r2, r0, r1
	cmp   r2, r3
	bcc   CopyDataInit

	/* Zero .bss */
	ldr   r2, =_sbss
	b     LoopFillZerobss

FillZerobss:
	movs  r3, #0
	str   r3, [r2], #4

LoopFillZerobss:
	ldr   r3, =_ebss
	cmp   r2, r3
	bcc   FillZerobss

	/* Second breadcrumb: startup completed, entering main() (slot 9). */
	ldr   r0, =g_pfnVectors
	ldr   r1, =0xB007C0DF
	str   r1, [r0, #0x24]
	dsb

	bl    main

LoopForever:
	b     LoopForever

	.size Reset_Handler, .-Reset_Handler

	.section .text.Default_Handler,"ax",%progbits
Default_Handler:
Infinite_Loop:
	b Infinite_Loop
	.size Default_Handler, .-Default_Handler

	.section .isr_vector,"a",%progbits
	.type g_pfnVectors, %object
g_pfnVectors:
	.word  _estack             /* Top of stack           */
	.word  Reset_Handler       /* Reset                  */
	.word  NMI_Handler         /* NMI                    */
	.word  HardFault_Handler   /* Hard fault             */
	.word  MemManage_Handler   /* MPU fault              */
	.word  BusFault_Handler    /* Bus fault              */
	.word  UsageFault_Handler  /* Usage fault            */
	.word  SecureFault_Handler /* Secure fault           */
	.word  0                   /* Reserved (boot breadcrumb 0) */
	.word  0                   /* Reserved (boot breadcrumb 1) */
	.word  0                   /* Reserved               */
	.word  SVC_Handler         /* SVCall                 */
	.word  DebugMon_Handler    /* Debug monitor          */
	.word  0                   /* Reserved               */
	.word  PendSV_Handler      /* PendSV                 */
	.word  SysTick_Handler     /* SysTick                */

	/* External interrupts 0..172 are unused by this demo. */
	.rept  173
	.word  Default_Handler
	.endr
	.word  IPCC1_RX_S_IRQHandler   /* IRQ 173: IPCC1 RX occupied, secure */

	.size  g_pfnVectors, .-g_pfnVectors

	.macro weak_handler name
	.weak      \name
	.thumb_set \name,Default_Handler
	.endm

	weak_handler NMI_Handler
	weak_handler HardFault_Handler
	weak_handler MemManage_Handler
	weak_handler BusFault_Handler
	weak_handler UsageFault_Handler
	weak_handler SecureFault_Handler
	weak_handler SVC_Handler
	weak_handler DebugMon_Handler
	weak_handler PendSV_Handler
	weak_handler SysTick_Handler

  /* External Interrupts: 
     C-linkage functions jump to a C++ callable-objects
	 See drivers/interrupt.hh
 */
  .word IRQ_Trampoline_0
  .word IRQ_Trampoline_1
  .word IRQ_Trampoline_2
  .word IRQ_Trampoline_3
  .word IRQ_Trampoline_4
  .word IRQ_Trampoline_5
  .word IRQ_Trampoline_6
  .word IRQ_Trampoline_7
  .word IRQ_Trampoline_8
  .word IRQ_Trampoline_9
  .word IRQ_Trampoline_10
  .word IRQ_Trampoline_11
  .word IRQ_Trampoline_12
  .word IRQ_Trampoline_13
  .word IRQ_Trampoline_14
  .word IRQ_Trampoline_15
  .word IRQ_Trampoline_16
  .word IRQ_Trampoline_17
  .word IRQ_Trampoline_18
  .word IRQ_Trampoline_19
  .word IRQ_Trampoline_20
  .word IRQ_Trampoline_21
  .word IRQ_Trampoline_22
  .word IRQ_Trampoline_23
  .word IRQ_Trampoline_24
  .word IRQ_Trampoline_25
  .word IRQ_Trampoline_26
  .word IRQ_Trampoline_27
  .word IRQ_Trampoline_28
  .word IRQ_Trampoline_29
  .word IRQ_Trampoline_30
  .word IRQ_Trampoline_31
  .word IRQ_Trampoline_32
  .word IRQ_Trampoline_33
  .word IRQ_Trampoline_34
  .word IRQ_Trampoline_35
  .word IRQ_Trampoline_36
  .word IRQ_Trampoline_37
  .word IRQ_Trampoline_38
  .word IRQ_Trampoline_39
  .word IRQ_Trampoline_40
  .word IRQ_Trampoline_41
  .word IRQ_Trampoline_42
  .word IRQ_Trampoline_43
  .word IRQ_Trampoline_44
  .word IRQ_Trampoline_45
  .word IRQ_Trampoline_46
  .word IRQ_Trampoline_47
  .word IRQ_Trampoline_48
  .word IRQ_Trampoline_49
  .word IRQ_Trampoline_50
  .word IRQ_Trampoline_51
  .word IRQ_Trampoline_52
  .word IRQ_Trampoline_53
  .word IRQ_Trampoline_54
  .word IRQ_Trampoline_55
  .word IRQ_Trampoline_56
  .word IRQ_Trampoline_57
  .word IRQ_Trampoline_58
  .word IRQ_Trampoline_59
  .word IRQ_Trampoline_60
  .word IRQ_Trampoline_61
  .word IRQ_Trampoline_62
  .word IRQ_Trampoline_63
  .word IRQ_Trampoline_64
  .word IRQ_Trampoline_65
  .word IRQ_Trampoline_66
  .word IRQ_Trampoline_67
  .word IRQ_Trampoline_68
  .word IRQ_Trampoline_69
  .word IRQ_Trampoline_70
  .word IRQ_Trampoline_71
  .word IRQ_Trampoline_72
  .word IRQ_Trampoline_73
  .word IRQ_Trampoline_74
  .word IRQ_Trampoline_75
  .word IRQ_Trampoline_76
  .word IRQ_Trampoline_77
  .word IRQ_Trampoline_78
  .word IRQ_Trampoline_79
  .word IRQ_Trampoline_80
  .word IRQ_Trampoline_81
  .word IRQ_Trampoline_82
  .word IRQ_Trampoline_83
  .word IRQ_Trampoline_84
  .word IRQ_Trampoline_85
  .word IRQ_Trampoline_86
  .word IRQ_Trampoline_87
  .word IRQ_Trampoline_88
  .word IRQ_Trampoline_89
  .word IRQ_Trampoline_90
  .word IRQ_Trampoline_91
  .word IRQ_Trampoline_92
  .word IRQ_Trampoline_93
  .word IRQ_Trampoline_94
  .word IRQ_Trampoline_95
  .word IRQ_Trampoline_96
  .word IRQ_Trampoline_97
  .word IRQ_Trampoline_98
  .word IRQ_Trampoline_99
  .word IRQ_Trampoline_100
  .word IRQ_Trampoline_101
  .word IRQ_Trampoline_102
  .word IRQ_Trampoline_103
  .word IRQ_Trampoline_104
  .word IRQ_Trampoline_105
  .word IRQ_Trampoline_106
  .word IRQ_Trampoline_107
  .word IRQ_Trampoline_108
  .word IRQ_Trampoline_109
  .word IRQ_Trampoline_110
  .word IRQ_Trampoline_111
  .word IRQ_Trampoline_112
  .word IRQ_Trampoline_113
  .word IRQ_Trampoline_114
  .word IRQ_Trampoline_115
  .word IRQ_Trampoline_116
  .word IRQ_Trampoline_117
  .word IRQ_Trampoline_118
  .word IRQ_Trampoline_119
  .word IRQ_Trampoline_120
  .word IRQ_Trampoline_121
  .word IRQ_Trampoline_122
  .word IRQ_Trampoline_123
  .word IRQ_Trampoline_124
  .word IRQ_Trampoline_125
  .word IRQ_Trampoline_126
  .word IRQ_Trampoline_127
  .word IRQ_Trampoline_128
  .word IRQ_Trampoline_129
  .word IRQ_Trampoline_130
  .word IRQ_Trampoline_131
  .word IRQ_Trampoline_132
  .word IRQ_Trampoline_133
  .word IRQ_Trampoline_134
  .word IRQ_Trampoline_135
  .word IRQ_Trampoline_136
  .word IRQ_Trampoline_137
  .word IRQ_Trampoline_138
  .word IRQ_Trampoline_139
  .word IRQ_Trampoline_140
  .word IRQ_Trampoline_141
  .word IRQ_Trampoline_142
  .word IRQ_Trampoline_143
  .word IRQ_Trampoline_144
  .word IRQ_Trampoline_145
  .word IRQ_Trampoline_146
  .word IRQ_Trampoline_147
  .word IRQ_Trampoline_148
  .word IRQ_Trampoline_149
  .word IRQ_Trampoline_150
  .word IRQ_Trampoline_151
  .word IRQ_Trampoline_152
  .word IRQ_Trampoline_153
  .word IRQ_Trampoline_154
  .word IRQ_Trampoline_155
  .word IRQ_Trampoline_156
  .word IRQ_Trampoline_157
  .word IRQ_Trampoline_158
  .word IRQ_Trampoline_159
  .word IRQ_Trampoline_160
  .word IRQ_Trampoline_161
  .word IRQ_Trampoline_162
  .word IRQ_Trampoline_163
  .word IRQ_Trampoline_164
  .word IRQ_Trampoline_165
  .word IRQ_Trampoline_166
  .word IRQ_Trampoline_167
  .word IRQ_Trampoline_168
  .word IRQ_Trampoline_169
  .word IRQ_Trampoline_170
  .word IRQ_Trampoline_171
  .word IRQ_Trampoline_172
  .word IRQ_Trampoline_173
  .word IRQ_Trampoline_174
  .word IRQ_Trampoline_175
  .word IRQ_Trampoline_176
  .word IRQ_Trampoline_177
  .word IRQ_Trampoline_178
  .word IRQ_Trampoline_179
  .word IRQ_Trampoline_180
  .word IRQ_Trampoline_181
  .word IRQ_Trampoline_182
  .word IRQ_Trampoline_183
  .word IRQ_Trampoline_184
  .word IRQ_Trampoline_185
  .word IRQ_Trampoline_186
  .word IRQ_Trampoline_187
  .word IRQ_Trampoline_188
  .word IRQ_Trampoline_189
  .word IRQ_Trampoline_190
  .word IRQ_Trampoline_191
  .word IRQ_Trampoline_192
  .word IRQ_Trampoline_193
  .word IRQ_Trampoline_194
  .word IRQ_Trampoline_195
  .word IRQ_Trampoline_196
  .word IRQ_Trampoline_197
  .word IRQ_Trampoline_198
  .word IRQ_Trampoline_199
  .word IRQ_Trampoline_200
  .word IRQ_Trampoline_201
  .word IRQ_Trampoline_202
  .word IRQ_Trampoline_203
  .word IRQ_Trampoline_204
  .word IRQ_Trampoline_205
  .word IRQ_Trampoline_206
  .word IRQ_Trampoline_207
  .word IRQ_Trampoline_208
  .word IRQ_Trampoline_209
  .word IRQ_Trampoline_210
  .word IRQ_Trampoline_211
  .word IRQ_Trampoline_212
  .word IRQ_Trampoline_213
  .word IRQ_Trampoline_214
  .word IRQ_Trampoline_215
  .word IRQ_Trampoline_216
  .word IRQ_Trampoline_217
  .word IRQ_Trampoline_218
  .word IRQ_Trampoline_219
  .word IRQ_Trampoline_220
  .word IRQ_Trampoline_221
  .word IRQ_Trampoline_222
  .word IRQ_Trampoline_223
  .word IRQ_Trampoline_224
  .word IRQ_Trampoline_225
  .word IRQ_Trampoline_226
  .word IRQ_Trampoline_227
  .word IRQ_Trampoline_228
  .word IRQ_Trampoline_229
  .word IRQ_Trampoline_230
  .word IRQ_Trampoline_231
  .word IRQ_Trampoline_232
  .word IRQ_Trampoline_233
  .word IRQ_Trampoline_234
  .word IRQ_Trampoline_235
  .word IRQ_Trampoline_236
  .word IRQ_Trampoline_237
  .word IRQ_Trampoline_238
  .word IRQ_Trampoline_239
  .word IRQ_Trampoline_240
  .word IRQ_Trampoline_241
  .word IRQ_Trampoline_242
  .word IRQ_Trampoline_243
  .word IRQ_Trampoline_244
  .word IRQ_Trampoline_245
  .word IRQ_Trampoline_246
  .word IRQ_Trampoline_247
  .word IRQ_Trampoline_248
  .word IRQ_Trampoline_249
  .word IRQ_Trampoline_250
  .word IRQ_Trampoline_251
  .word IRQ_Trampoline_252
  .word IRQ_Trampoline_253
  .word IRQ_Trampoline_254
  .word IRQ_Trampoline_255
  .word IRQ_Trampoline_256
  .word IRQ_Trampoline_257
  .word IRQ_Trampoline_258
  .word IRQ_Trampoline_259
  .word IRQ_Trampoline_260
  .word IRQ_Trampoline_261
  .word IRQ_Trampoline_262
  .word IRQ_Trampoline_263
  .word IRQ_Trampoline_264
  .word IRQ_Trampoline_265
  .word IRQ_Trampoline_266
  .word IRQ_Trampoline_267
  .word IRQ_Trampoline_268
  .word IRQ_Trampoline_269
  .word IRQ_Trampoline_270
  .word IRQ_Trampoline_271
  .word IRQ_Trampoline_272
  .word IRQ_Trampoline_273
  .word IRQ_Trampoline_274
  .word IRQ_Trampoline_275
  .word IRQ_Trampoline_276
  .word IRQ_Trampoline_277
  .word IRQ_Trampoline_278
  .word IRQ_Trampoline_279
  .word IRQ_Trampoline_280
  .word IRQ_Trampoline_281
  .word IRQ_Trampoline_282
  .word IRQ_Trampoline_283
  .word IRQ_Trampoline_284
  .word IRQ_Trampoline_285
  .word IRQ_Trampoline_286
  .word IRQ_Trampoline_287
  .word IRQ_Trampoline_288
  .word IRQ_Trampoline_289
  .word IRQ_Trampoline_290
  .word IRQ_Trampoline_291
  .word IRQ_Trampoline_292
  .word IRQ_Trampoline_293
  .word IRQ_Trampoline_294
  .word IRQ_Trampoline_295
  .word IRQ_Trampoline_296
  .word IRQ_Trampoline_297
  .word IRQ_Trampoline_298
  .word IRQ_Trampoline_299
  .word IRQ_Trampoline_300
  .word IRQ_Trampoline_301
  .word IRQ_Trampoline_302
  .word IRQ_Trampoline_303
  .word IRQ_Trampoline_304
  .word IRQ_Trampoline_305
  .word IRQ_Trampoline_306
  .word IRQ_Trampoline_307
  .word IRQ_Trampoline_308
  .word IRQ_Trampoline_309
  .word IRQ_Trampoline_310
  .word IRQ_Trampoline_311
  .word IRQ_Trampoline_312
  .word IRQ_Trampoline_313
  .word IRQ_Trampoline_314
  .word IRQ_Trampoline_315
  .word IRQ_Trampoline_316
  .word IRQ_Trampoline_317
  .word IRQ_Trampoline_318
  .word IRQ_Trampoline_319
  .word IRQ_Trampoline_320
  .word IRQ_Trampoline_321
  .word IRQ_Trampoline_322
  .word IRQ_Trampoline_323
  .word IRQ_Trampoline_324
  .word IRQ_Trampoline_325
  .word IRQ_Trampoline_326
  .word IRQ_Trampoline_327
  .word IRQ_Trampoline_328
  .word IRQ_Trampoline_329
  .word IRQ_Trampoline_330
  .word IRQ_Trampoline_331
  .word IRQ_Trampoline_332
  .word IRQ_Trampoline_333
  .word IRQ_Trampoline_334
  .word IRQ_Trampoline_335
  .word IRQ_Trampoline_336
  .word IRQ_Trampoline_337
  .word IRQ_Trampoline_338
  .word IRQ_Trampoline_339
  .word IRQ_Trampoline_340
  .word IRQ_Trampoline_341
  .word IRQ_Trampoline_342
  .word IRQ_Trampoline_343
  .word IRQ_Trampoline_344
  .word IRQ_Trampoline_345
  .word IRQ_Trampoline_346
  .word IRQ_Trampoline_347
  .word IRQ_Trampoline_348
  .word IRQ_Trampoline_349
  .word IRQ_Trampoline_350
  .word IRQ_Trampoline_351
  .word IRQ_Trampoline_352
  .word IRQ_Trampoline_353
  .word IRQ_Trampoline_354
  .word IRQ_Trampoline_355
  .word IRQ_Trampoline_356
  .word IRQ_Trampoline_357
  .word IRQ_Trampoline_358
  .word IRQ_Trampoline_359
  .word IRQ_Trampoline_360
  .word IRQ_Trampoline_361
  .word IRQ_Trampoline_362
  .word IRQ_Trampoline_363
  .word IRQ_Trampoline_364
  .word IRQ_Trampoline_365
  .word IRQ_Trampoline_366
  .word IRQ_Trampoline_367
  .word IRQ_Trampoline_368
  .word IRQ_Trampoline_369
  .word IRQ_Trampoline_370
  .word IRQ_Trampoline_371
  .word IRQ_Trampoline_372
  .word IRQ_Trampoline_373
  .word IRQ_Trampoline_374
  .word IRQ_Trampoline_375

