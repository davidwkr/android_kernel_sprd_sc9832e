# 4.4 headers the camera drivers still include: sprd-glb.h with the sharkle
# register definitions, and the linux/wakelock.h wrapper on wakeup sources.
subdir-ccflags-y += -I$(srctree)/drivers/misc/sprd_camera/compat

obj-$(CONFIG_SPRD_CAMERA) += isp/
obj-$(CONFIG_SPRD_CAMERA) += sensor/
obj-$(CONFIG_SPRD_CAMERA) += dcam/
obj-$(CONFIG_SPRD_CAMERA) += cpp/
obj-$(CONFIG_SPRD_CAMERA) += flash/
obj-$(CONFIG_SPRD_CAMERA) += common/

