/* GPU firmware for the shim bring-up (research/gpu/fw is gitignored: never commit the blobs). */
	.section .rodata
	.balign 16
	.global gmu_fw, gmu_fw_end
gmu_fw:
	.incbin "../research/gpu/fw/linux-firmware/a630_gmu.bin"
gmu_fw_end:
	.balign 16
	.global sqe_fw, sqe_fw_end
sqe_fw:
	.incbin "../research/gpu/fw/linux-firmware/a630_sqe.fw"
sqe_fw_end:
	.balign 16
	.global zap_mdt, zap_mdt_end
zap_mdt:
	.incbin "../research/gpu/fw/a615_zap.mdt"
zap_mdt_end:
	.balign 16
	.global zap_b02, zap_b02_end
zap_b02:
	.incbin "../research/gpu/fw/a615_zap.b02"
zap_b02_end:
