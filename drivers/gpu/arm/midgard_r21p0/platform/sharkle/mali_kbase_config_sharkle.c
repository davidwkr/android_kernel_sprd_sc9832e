/*
 *
 * (C) COPYRIGHT ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */



#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/hwspinlock.h>
#include <mali_kbase.h>
#include <mali_kbase_defs.h>
#include <mali_kbase_config.h>
#include <mali_kbase_debug.h>
#include <backend/gpu/mali_kbase_device_internal.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of_address.h>
#ifdef KBASE_PM_RUNTIME
#include <linux/pm_runtime.h>
#endif
#ifdef CONFIG_OF
#include <linux/of.h>
#endif
#ifdef CONFIG_MALI_HOTPLUG
#include <hotplug/mali_kbase_hotplug.h>
#endif /* CONFIG_MALI_HOTPLUG */
#include <linux/regulator/consumer.h>

#include <linux/smp.h>
#include <trace/events/power.h>
#define VENDOR_FTRACE_MODULE_NAME    "unisoc-gpu"

#define DTS_CLK_OFFSET      2
#define PM_RUNTIME_DELAY_MS 50
#define UP_THRESHOLD        9/10

/*
 * GPU power-domain register in the pmu-apb syscon block (PD_GPU_TOP_CFG),
 * offset 0x20. Bits [25:24] are two independent control bits, not an
 * encoded 2-bit field: bit 25 forces the domain off regardless of other
 * state, bit 24 lets the domain shut itself down automatically. The upstream
 * ARM reference "sharkle" platform
 * (drivers/gpu/arm/midgard/mali/platform/sharkle in the r28p0 copy in this
 * tree) gets this reg/mask pair from the DT via syscon-names =
 * "top_force_shutdown" on the gpu node, with the generic
 * arch/arm64/boot/dts/sprd/sharkle.dtsi reference using "syscons =
 * <&pmu_apb_controller 0x20 0x2000000>" - i.e. mask 0x2000000, touching only
 * the force-shutdown bit (25) and leaving automatic shutdown (24) alone.
 * Neither the k18 nor the dm63 reconstructed stock DTS carries that
 * "syscons"/"syscon-names" property (only "sprd,syscon-aon-apb" /
 * "sprd,syscon-pmu-apb" phandles), so the reg/mask here are spelled out as
 * constants instead of read from DT. This port intentionally uses the wider
 * mask 0x3000000, clearing *both* bits on power-on: that selects a fully
 * software-managed domain (no surprise auto-shutdown behind the driver's
 * back) rather than just disabling the hard force-off, matching what the
 * stock k18 mali.ko does (see below) - it is not the same value as the
 * generic sharkle.dtsi reference above, and that reference's narrower mask
 * should not be read as corroborating this one. Confirmed against the stock
 * k18 mali.ko (strings/objdump): it resolves this same register via
 * syscon_regmap_lookup_by_phandle(..., "sprd,syscon-pmu-apb") and an
 * ioremap'd access at (phandle base + 0x20), matching this offset. See
 * devices/MALI_R21P0.md for the full trail of evidence.
 */
#define GPU_PWR_DOMAIN_REG   0x20
#define GPU_PWR_DOMAIN_MASK  0x3000000
#define GPU_PWR_DOMAIN_ON    0x0
#define GPU_PWR_DOMAIN_OFF   0x2000000

struct gpu_freq_info {
	struct clk* clk_src;
	int freq;	//kHz
	int volt;	//uV
	int div;
	int up_threshold;
};

struct gpu_reg_info {
	struct regmap* regmap_ptr;
	uint32_t args[2];
};

struct gpu_dfs_context {
	int gpu_clock_on;
	int gpu_power_on;
	int cur_load;
	int cur_voltage;

	struct clk*  gpu_clock;
	struct clk* gpu_clock_i;
	struct clk** gpu_clk_src;
	int gpu_clk_num;

	struct gpu_freq_info* freq_list;
	int freq_list_len;

	const struct gpu_freq_info* freq_cur;
	const struct gpu_freq_info* freq_next;

	const struct gpu_freq_info* freq_min;
	const struct gpu_freq_info* freq_max;
	const struct gpu_freq_info* freq_default;
	const struct gpu_freq_info* freq_9;
	const struct gpu_freq_info* freq_8;
	const struct gpu_freq_info* freq_7;
	const struct gpu_freq_info* freq_5;
	const struct gpu_freq_info* freq_range_max;
	const struct gpu_freq_info* freq_range_min;

	struct workqueue_struct *gpu_dfs_workqueue;
	struct semaphore* sem;

	struct gpu_reg_info top_reg;
};

#ifndef CONFIG_OF
static struct kbase_io_resources io_resources = {
	.job_irq_number = 68,
	.mmu_irq_number = 69,
	.gpu_irq_number = 70,
	.io_memory_region = {
	.start = 0x2F010000,
	.end = 0x2F010000 + (4096 * 4) - 1}
};
#endif

DEFINE_SEMAPHORE(gpu_dfs_sem);
static struct gpu_dfs_context gpu_dfs_ctx=
{
	.gpu_clock_on=0,
	.gpu_power_on=0,

	.sem=&gpu_dfs_sem,
};

static struct regulator *gpu_reg = NULL;

extern int gpu_boost_level;

extern int gpu_freq_cur;
extern int gpu_voltage_cur;
#ifdef CONFIG_MALI_MIDGARD_DVFS
extern int gpu_freq_min_limit;
extern int gpu_freq_max_limit;
#endif
extern char* gpu_freq_list;

static void gpu_freq_list_show(char* buf)
{
	int i=0,len=0;

	for(i=0; i<gpu_dfs_ctx.freq_list_len; i++)
	{
		len = sprintf(buf,"%2d  %6d kHz %6d uV\n", i, gpu_dfs_ctx.freq_list[i].freq, gpu_dfs_ctx.freq_list[i].volt);
		buf += len;
	}
}

#define GPU_SUPPORT_QOS
#ifdef GPU_SUPPORT_QOS

#define ARQOS_THRESHOLD_MASK  0xFUL
#define AWQOS_THRESHOLD_MASK  0xFUL

static volatile void __iomem* gpu_qos_reg = NULL;

static int   gpu_always_on = 0;
module_param_named(gpu_always_on, gpu_always_on, int, S_IRUGO | S_IWUSR);

static int   gpu_qos_threshold_aw = 6;
module_param_named(gpu_qos_threshold_aw, gpu_qos_threshold_aw, int, S_IRUGO | S_IWUSR);

static int   gpu_qos_threshold_ar = 6;
module_param_named(gpu_qos_threshold_ar, gpu_qos_threshold_ar, int, S_IRUGO | S_IWUSR);

static void config_gpu_qos(void)
{
	uint32_t reg_val = 0;
	reg_val = readl(gpu_qos_reg);
	reg_val &= ~(ARQOS_THRESHOLD_MASK << 8);
	reg_val |= gpu_qos_threshold_ar << 8 ;

	reg_val &= ~(AWQOS_THRESHOLD_MASK << 12);
	reg_val |= gpu_qos_threshold_aw << 12 ;
	writel(reg_val, gpu_qos_reg);

	KBASE_DEBUG_PRINT(4, "%s: gpu qos reg:0x%x \n", __func__,  readl(gpu_qos_reg));
}
#endif

/*
 * mali_freq_init() used to probe every DT resource through
 * KBASE_DEBUG_ASSERT(), which compiles to a no-op in production builds
 * (CONFIG_MALI_DEBUG is off in both k18_defconfig and dm63_defconfig): a
 * missing syscon, clock or allocation was silently ignored and execution
 * fell through into mali_power_on()/mali_clock_on() with an IS_ERR or NULL
 * pointer. Each lookup below is now checked for real, with IS_ERR() (not a
 * NULL check) for the clk/regmap APIs, which return ERR_PTR() - including
 * -EPROBE_DEFER when the syscon or clock provider hasn't probed yet, now
 * propagated to the caller instead of swallowed. On failure, everything
 * already acquired is unwound so a deferred probe can retry cleanly.
 *
 * The "sprd,dfs-*" index properties parsed further down are still read
 * without validating each of_property_read_u32() call or bounds-checking
 * the resulting freq_list[] index - that is a devicetree-authoring
 * correctness concern (this DT is ours, not attacker input), left as a
 * hardware bring-up item; see devices/MALI_R21P0.md.
 */
static inline int mali_freq_init(struct device *dev)
{
#ifdef CONFIG_OF
	int i = 0, clk_cnt = 0, got_clk_src = 0;
	int err;

#ifdef GPU_SUPPORT_QOS
	gpu_qos_reg = ioremap_nocache( 0x60100014, 4);
	if (!gpu_qos_reg)
		return -ENOMEM;
#endif

	gpu_dfs_ctx.top_reg.regmap_ptr = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,syscon-pmu-apb");
	if (IS_ERR(gpu_dfs_ctx.top_reg.regmap_ptr)) {
		err = PTR_ERR(gpu_dfs_ctx.top_reg.regmap_ptr);
		goto err_qos;
	}
	gpu_dfs_ctx.top_reg.args[0] = GPU_PWR_DOMAIN_REG;
	gpu_dfs_ctx.top_reg.args[1] = GPU_PWR_DOMAIN_MASK;

	gpu_dfs_ctx.gpu_clock_i = of_clk_get(dev->of_node, 0);
	if (IS_ERR(gpu_dfs_ctx.gpu_clock_i)) {
		err = PTR_ERR(gpu_dfs_ctx.gpu_clock_i);
		goto err_qos;
	}
	gpu_dfs_ctx.gpu_clock = of_clk_get(dev->of_node, 1);
	if (IS_ERR(gpu_dfs_ctx.gpu_clock)) {
		err = PTR_ERR(gpu_dfs_ctx.gpu_clock);
		goto err_clock_i;
	}

	clk_cnt = of_clk_get_parent_count(dev->of_node);
	gpu_dfs_ctx.gpu_clk_num = clk_cnt - DTS_CLK_OFFSET;
	if (gpu_dfs_ctx.gpu_clk_num <= 0) {
		err = -ENODEV;
		goto err_clock;
	}

	gpu_dfs_ctx.gpu_clk_src = vmalloc(sizeof(struct clk*) * gpu_dfs_ctx.gpu_clk_num);
	if (!gpu_dfs_ctx.gpu_clk_src) {
		err = -ENOMEM;
		goto err_clock;
	}

	for (i = 0; i < gpu_dfs_ctx.gpu_clk_num; i++)
	{
		gpu_dfs_ctx.gpu_clk_src[i] = of_clk_get(dev->of_node, i+DTS_CLK_OFFSET);
		if (IS_ERR(gpu_dfs_ctx.gpu_clk_src[i])) {
			err = PTR_ERR(gpu_dfs_ctx.gpu_clk_src[i]);
			goto err_clk_src;
		}
		got_clk_src++;
	}

	gpu_dfs_ctx.freq_list_len = of_property_count_elems_of_size(dev->of_node,"sprd,dfs-lists",4*sizeof(u32));
	if (gpu_dfs_ctx.freq_list_len <= 0) {
		err = (gpu_dfs_ctx.freq_list_len < 0) ? gpu_dfs_ctx.freq_list_len : -EINVAL;
		goto err_clk_src;
	}
	gpu_dfs_ctx.freq_list = vmalloc(sizeof(struct gpu_freq_info) * gpu_dfs_ctx.freq_list_len);
	if (!gpu_dfs_ctx.freq_list) {
		err = -ENOMEM;
		goto err_clk_src;
	}

	for(i=0; i<gpu_dfs_ctx.freq_list_len; i++)
	{
		int clk = 0;

		of_property_read_u32_index(dev->of_node, "sprd,dfs-lists", 4*i+2, &clk);
		/* absolute index into "clocks": a parent, after gate and mux */
		if (clk < DTS_CLK_OFFSET ||
		    clk - DTS_CLK_OFFSET >= gpu_dfs_ctx.gpu_clk_num) {
			dev_err(dev, "sprd,dfs-lists entry %d: bad clock index %d\n",
				i, clk);
			err = -EINVAL;
			vfree(gpu_dfs_ctx.freq_list);
			gpu_dfs_ctx.freq_list = NULL;
			goto err_clk_src;
		}
		gpu_dfs_ctx.freq_list[i].clk_src = gpu_dfs_ctx.gpu_clk_src[clk-DTS_CLK_OFFSET];
		KBASE_DEBUG_ASSERT(gpu_dfs_ctx.freq_list[i].clk_src);
		of_property_read_u32_index(dev->of_node, "sprd,dfs-lists", 4*i, &gpu_dfs_ctx.freq_list[i].freq);
		of_property_read_u32_index(dev->of_node, "sprd,dfs-lists", 4*i+1, &gpu_dfs_ctx.freq_list[i].volt);
		of_property_read_u32_index(dev->of_node, "sprd,dfs-lists", 4*i+3, &gpu_dfs_ctx.freq_list[i].div);
		gpu_dfs_ctx.freq_list[i].up_threshold =  gpu_dfs_ctx.freq_list[i].freq * UP_THRESHOLD;
	}

	of_property_read_u32(dev->of_node, "sprd,dfs-default", &i);
	gpu_dfs_ctx.freq_default = &gpu_dfs_ctx.freq_list[i];
	KBASE_DEBUG_ASSERT(gpu_dfs_ctx.freq_default);

	of_property_read_u32(dev->of_node, "sprd,dfs-scene-extreme", &i);
	gpu_dfs_ctx.freq_9 = &gpu_dfs_ctx.freq_list[i];
	KBASE_DEBUG_ASSERT(gpu_dfs_ctx.freq_9);

	of_property_read_u32(dev->of_node, "sprd,dfs-scene-high", &i);
	gpu_dfs_ctx.freq_8 = &gpu_dfs_ctx.freq_list[i];
	KBASE_DEBUG_ASSERT(gpu_dfs_ctx.freq_8);

	of_property_read_u32(dev->of_node, "sprd,dfs-scene-medium", &i);
	gpu_dfs_ctx.freq_7 = &gpu_dfs_ctx.freq_list[i];
	KBASE_DEBUG_ASSERT(gpu_dfs_ctx.freq_7);

	of_property_read_u32(dev->of_node, "sprd,dfs-scene-low", &i);
	gpu_dfs_ctx.freq_5 = &gpu_dfs_ctx.freq_list[i];
	KBASE_DEBUG_ASSERT(gpu_dfs_ctx.freq_5);

	of_property_read_u32(dev->of_node, "sprd,dfs-range-max", &i);
	gpu_dfs_ctx.freq_range_max = &gpu_dfs_ctx.freq_list[i];
	KBASE_DEBUG_ASSERT(gpu_dfs_ctx.freq_range_max);

	of_property_read_u32(dev->of_node, "sprd,dfs-range-min", &i);
	gpu_dfs_ctx.freq_range_min = &gpu_dfs_ctx.freq_list[i];
	KBASE_DEBUG_ASSERT(gpu_dfs_ctx.freq_range_min);

	gpu_dfs_ctx.freq_max = gpu_dfs_ctx.freq_range_max;
	gpu_dfs_ctx.freq_min = gpu_dfs_ctx.freq_range_min;
	gpu_dfs_ctx.freq_cur = gpu_dfs_ctx.freq_default;
	gpu_dfs_ctx.cur_voltage = gpu_voltage_cur = gpu_dfs_ctx.freq_cur->volt;

	return 0;

err_clk_src:
	for (i = got_clk_src - 1; i >= 0; i--)
		clk_put(gpu_dfs_ctx.gpu_clk_src[i]);
	vfree(gpu_dfs_ctx.gpu_clk_src);
	gpu_dfs_ctx.gpu_clk_src = NULL;
err_clock:
	clk_put(gpu_dfs_ctx.gpu_clock);
err_clock_i:
	clk_put(gpu_dfs_ctx.gpu_clock_i);
err_qos:
#ifdef GPU_SUPPORT_QOS
	if (gpu_qos_reg) {
		iounmap(gpu_qos_reg);
		gpu_qos_reg = NULL;
	}
#endif
	return err;
#else
	return 0;
#endif
}

/* Undo a successful mali_freq_init(), for the mali_platform_init() error
 * path where a later step (power/clock/regulator) fails. */
static inline void mali_freq_term(void)
{
#ifdef CONFIG_OF
	int i;

	vfree(gpu_dfs_ctx.freq_list);
	gpu_dfs_ctx.freq_list = NULL;

	if (gpu_dfs_ctx.gpu_clk_src) {
		for (i = 0; i < gpu_dfs_ctx.gpu_clk_num; i++)
			clk_put(gpu_dfs_ctx.gpu_clk_src[i]);
		vfree(gpu_dfs_ctx.gpu_clk_src);
		gpu_dfs_ctx.gpu_clk_src = NULL;
	}

	clk_put(gpu_dfs_ctx.gpu_clock);
	clk_put(gpu_dfs_ctx.gpu_clock_i);

#ifdef GPU_SUPPORT_QOS
	if (gpu_qos_reg) {
		iounmap(gpu_qos_reg);
		gpu_qos_reg = NULL;
	}
#endif
#endif
}

static inline int mali_power_on(void)
{
	int err;

	err = regmap_update_bits(gpu_dfs_ctx.top_reg.regmap_ptr, gpu_dfs_ctx.top_reg.args[0], gpu_dfs_ctx.top_reg.args[1], GPU_PWR_DOMAIN_ON);
	if (err) {
		pr_err("mali: failed to power on GPU domain (%d)\n", err);
		return err;
	}

	udelay(300);
	gpu_dfs_ctx.gpu_power_on = 1;

	return 0;
}

static inline void mali_power_off(void)
{
#ifdef GPU_SUPPORT_QOS
	if(gpu_always_on)
		return;
#endif
	gpu_dfs_ctx.gpu_power_on = 0;

	regmap_update_bits(gpu_dfs_ctx.top_reg.regmap_ptr, gpu_dfs_ctx.top_reg.args[0], gpu_dfs_ctx.top_reg.args[1], GPU_PWR_DOMAIN_OFF);
}

static inline void mali_set_div(const struct gpu_freq_info* info)
{
	//clk_set_rate(info->clk_src, info->freq/info->div);
}

static inline int mali_clock_on(void)
{
	int i, err;

	//enable all clocks
	for(i=0;i<gpu_dfs_ctx.gpu_clk_num;i++)
	{
		err = clk_prepare_enable(gpu_dfs_ctx.gpu_clk_src[i]);
		if (err)
			goto err_clk_src;
	}

	err = clk_prepare_enable(gpu_dfs_ctx.gpu_clock_i);
	if (err)
		goto err_clk_src;

	//enable gpu clock
	err = clk_prepare_enable(gpu_dfs_ctx.gpu_clock);
	if (err)
		goto err_clock_i;
	udelay(300);

	//set gpu clock parent
	err = clk_set_parent(gpu_dfs_ctx.gpu_clock, gpu_dfs_ctx.freq_default->clk_src);
	if (err)
		goto err_gpu_clock;

	KBASE_DEBUG_ASSERT(gpu_dfs_ctx.freq_cur);
	err = clk_set_rate(gpu_dfs_ctx.freq_cur->clk_src, gpu_dfs_ctx.freq_cur->freq*1000);
	if (!err)
		err = clk_set_parent(gpu_dfs_ctx.gpu_clock, gpu_dfs_ctx.freq_cur->clk_src);
	if (err) {
		/*
		 * Stay on the default parent (the lowest-voltage OPP, so safe
		 * at any recorded voltage) and record it, instead of running
		 * a parent whose rate was not established.
		 */
		pr_err("mali: GPU clock restore to %d kHz failed (%d), using %d kHz\n",
		       gpu_dfs_ctx.freq_cur->freq, err,
		       gpu_dfs_ctx.freq_default->freq);
		clk_set_parent(gpu_dfs_ctx.gpu_clock, gpu_dfs_ctx.freq_default->clk_src);
		gpu_dfs_ctx.freq_cur = gpu_dfs_ctx.freq_default;
		err = 0;
	}
	mali_set_div(gpu_dfs_ctx.freq_cur);

#ifdef GPU_SUPPORT_QOS
	config_gpu_qos();
#endif
	gpu_dfs_ctx.gpu_clock_on = 1;

	gpu_freq_cur = gpu_dfs_ctx.freq_cur->freq;

	return 0;

err_gpu_clock:
	clk_disable_unprepare(gpu_dfs_ctx.gpu_clock);
err_clock_i:
	clk_disable_unprepare(gpu_dfs_ctx.gpu_clock_i);
err_clk_src:
	pr_err("mali: failed to enable GPU clocks (%d)\n", err);
	for (i = i - 1; i >= 0; i--)
		clk_disable_unprepare(gpu_dfs_ctx.gpu_clk_src[i]);
	return err;
}

static inline void mali_clock_off(void)
{
	int i;

#ifdef GPU_SUPPORT_QOS
	if(gpu_always_on)
		return;
#endif
	gpu_freq_cur = 0;

	gpu_dfs_ctx.gpu_clock_on = 0;

	//disable gpu clock
	clk_disable_unprepare(gpu_dfs_ctx.gpu_clock);
	clk_disable_unprepare(gpu_dfs_ctx.gpu_clock_i);

	//disable all clocks
	for(i=0;i<gpu_dfs_ctx.gpu_clk_num;i++)
	{
		clk_disable_unprepare(gpu_dfs_ctx.gpu_clk_src[i]);
	}
}

static int mali_platform_init(struct kbase_device *kbdev)
{
	int err;

	KBASE_DEBUG_PRINT(4, "mali------------------- mali_platform_init\n");
	//gpu freq
	err = mali_freq_init(kbdev->dev);
	if (err)
		return err;

	err = mali_power_on();
	if (err)
		goto err_freq;

	//clock on
	err = mali_clock_on();
	if (err)
		goto err_power;

#ifdef CONFIG_MALI_HOTPLUG
	kbase_hotplug_wake_up();
#endif

	gpu_dfs_ctx.gpu_dfs_workqueue = create_singlethread_workqueue("gpu_dfs");
	if (!gpu_dfs_ctx.gpu_dfs_workqueue) {
		err = -ENOMEM;
		goto err_clock;
	}

	gpu_freq_list = (char*)vmalloc(256*sizeof(char));
	if (!gpu_freq_list) {
		err = -ENOMEM;
		goto err_workqueue;
	}
	gpu_freq_list_show(gpu_freq_list);

	//init gpu regulator
	gpu_reg = devm_regulator_get(kbdev->dev, "gpu");
	if (IS_ERR(gpu_reg)) {
		err = PTR_ERR(gpu_reg);
		gpu_reg = NULL;
		goto err_freq_list;
	}
#ifdef CONFIG_INIT_VOLTAGE
	regulator_set_voltage(gpu_reg, gpu_dfs_ctx.freq_cur->volt, gpu_dfs_ctx.freq_cur->volt);
#endif
#ifdef CONFIG_ADJUST_VOLTAGE
	/*
	 * cur_voltage was the default OPP's voltage, assumed. Use the rail's
	 * real voltage, and raise it if it is below the default OPP's (or
	 * unreadable); the GPU clock already runs at the default OPP, so a
	 * failed raise fails the probe.
	 */
	err = regulator_get_voltage(gpu_reg);
	gpu_dfs_ctx.cur_voltage = err > 0 ? err : 0;
	if (gpu_dfs_ctx.cur_voltage < gpu_dfs_ctx.freq_cur->volt) {
		err = regulator_set_voltage(gpu_reg, gpu_dfs_ctx.freq_cur->volt,
					    gpu_dfs_ctx.freq_cur->volt);
		if (err) {
			pr_err("mali: cannot set gpu voltage %d uV (%d)\n",
			       gpu_dfs_ctx.freq_cur->volt, err);
			goto err_freq_list;
		}
		gpu_dfs_ctx.cur_voltage = gpu_dfs_ctx.freq_cur->volt;
	}
	gpu_voltage_cur = gpu_dfs_ctx.cur_voltage;
	err = 0;
#endif

	return 0;

err_freq_list:
	vfree(gpu_freq_list);
	gpu_freq_list = NULL;
err_workqueue:
	destroy_workqueue(gpu_dfs_ctx.gpu_dfs_workqueue);
	gpu_dfs_ctx.gpu_dfs_workqueue = NULL;
err_clock:
	mali_clock_off();
err_power:
	mali_power_off();
err_freq:
	mali_freq_term();

	return err;
}

static void mali_platform_term(struct kbase_device *kbdev)
{
	KBASE_DEBUG_PRINT(4, "mali------------------- mali_platform_term\n");
	down(gpu_dfs_ctx.sem);
	//destory work queue
	destroy_workqueue(gpu_dfs_ctx.gpu_dfs_workqueue);

	//clock off
	mali_clock_off();

	//power off
	mali_power_off();

	//free
	vfree(gpu_freq_list);
	mali_freq_term();

	/* gpu_reg was obtained with devm_regulator_get(); devres already
	 * releases it when the device detaches, so don't also regulator_put()
	 * it here - doing both double-frees the regulator handle. */

#ifdef GPU_SUPPORT_QOS
	if(gpu_qos_reg)
	{
		iounmap(gpu_qos_reg);
		gpu_qos_reg = NULL;
	}
#endif

	up(gpu_dfs_ctx.sem);
}

struct kbase_platform_funcs_conf platform_sharkle_funcs = {
	.platform_init_func = mali_platform_init,
	.platform_term_func = mali_platform_term
};

static int mali_power_mode_change(struct kbase_device *kbdev, int power_mode)
{
	int err = 0;

	down(gpu_dfs_ctx.sem);
	KBASE_DEBUG_PRINT(3, "mali_power_mode_change: %d, gpu_power_on=%d gpu_clock_on=%d\n",power_mode,gpu_dfs_ctx.gpu_power_on,gpu_dfs_ctx.gpu_clock_on);
	switch (power_mode)
	{
		case 0://power on
			if (!gpu_dfs_ctx.gpu_power_on)
			{
				err = mali_power_on();
				if (!err)
					err = mali_clock_on();
			}

			if (!err && !gpu_dfs_ctx.gpu_clock_on)
			{
				err = mali_clock_on();
			}
#ifdef CONFIG_MALI_HOTPLUG
			if (!err)
				kbase_hotplug_wake_up();
#endif
			break;

		case 1://light sleep
		case 2://deep sleep
			if(gpu_dfs_ctx.gpu_clock_on)
			{
				mali_clock_off();
			}

			if(gpu_dfs_ctx.gpu_power_on)
			{
				mali_power_off();
			}
			break;

		default:
			break;
	}
	up(gpu_dfs_ctx.sem);

	return err;
}

static void pm_callback_power_off(struct kbase_device *kbdev)
{
#ifdef KBASE_PM_RUNTIME
	int res;

	KBASE_DEBUG_PRINT(4, "mali------------------- pm_callback_power_off\n");
	res = pm_runtime_put_sync(kbdev->dev);
	if (res < 0)
	{
		printk(KERN_ERR "mali----pm_runtime_put_sync return (%d)\n", res);
	}
#endif

	mali_power_mode_change(kbdev, 1);
}

static int pm_callback_power_on(struct kbase_device *kbdev)
{
	int err;

	KBASE_DEBUG_PRINT(4, "mali------------------- pm_callback_power_on\n");
	err = mali_power_mode_change(kbdev, 0);
	if (err)
		dev_err(kbdev->dev, "mali: GPU power-on sequence failed (%d)\n", err);

#ifdef KBASE_PM_RUNTIME
	{
		int res;

		res = pm_runtime_get_sync(kbdev->dev);
		if (res < 0)
		{
			printk(KERN_ERR "mali----pm_runtime_get_sync return (%d)\n", res);
		}
	}
#endif

	return 1;
}

static void pm_callback_power_suspend(struct kbase_device *kbdev)
{
	KBASE_DEBUG_PRINT(4, "mali------------------- pm_callback_power_suspend\n");
	mali_power_mode_change(kbdev, 2);
}

static void pm_callback_power_resume(struct kbase_device *kbdev)
{
	int err;

	KBASE_DEBUG_PRINT(4, "mali------------------- pm_callback_power_resume\n");
	err = mali_power_mode_change(kbdev, 0);
	if (err)
		dev_err(kbdev->dev, "mali: GPU power-on sequence failed (%d)\n", err);
}

#ifdef KBASE_PM_RUNTIME
static int pm_callback_power_runtime_init(struct kbase_device *kbdev)
{
	KBASE_DEBUG_PRINT(4, "mali------------------- pm_callback_power_runtime_init\n");
	pm_runtime_set_active(kbdev->dev);
	pm_suspend_ignore_children(kbdev->dev, true);
	pm_runtime_set_autosuspend_delay(kbdev->dev, PM_RUNTIME_DELAY_MS);
	pm_runtime_use_autosuspend(kbdev->dev);
	pm_runtime_enable(kbdev->dev);

	return 0;
}

static void pm_callback_power_runtime_term(struct kbase_device *kbdev)
{
	KBASE_DEBUG_PRINT(4, "mali------------------- pm_callback_power_runtime_term\n");
	pm_runtime_disable(kbdev->dev);
}
#endif/*CONFIG_PM_RUNTIME*/

struct kbase_pm_callback_conf pm_sharkle_callbacks = {
	.power_off_callback = pm_callback_power_off,
	.power_on_callback = pm_callback_power_on,
	.power_suspend_callback = pm_callback_power_suspend,
	.power_resume_callback = pm_callback_power_resume,
#ifdef KBASE_PM_RUNTIME
	.power_runtime_init_callback = pm_callback_power_runtime_init,
	.power_runtime_term_callback = pm_callback_power_runtime_term,
	.power_runtime_off_callback = NULL,
	.power_runtime_on_callback = NULL
#endif
};


static struct kbase_platform_config versatile_platform_config = {
#ifndef CONFIG_OF
	.io_resources = &io_resources
#endif
};

struct kbase_platform_config *kbase_get_platform_config(void)
{
	return &versatile_platform_config;
}

int kbase_platform_early_init(void)
{
	/* Nothing needed at this stage */
	return 0;
}

#if defined(CONFIG_MALI_MIDGARD_DVFS) || defined(CONFIG_MALI_DEVFREQ)
#ifdef CONFIG_ADJUST_VOLTAGE
static bool upgrade_voltage(void)
{
	bool result = true;
	int reg_ret = 0, target_voltage = 0;

	target_voltage = gpu_dfs_ctx.freq_next->volt;
	KBASE_DEBUG_PRINT(3, "GPU_DVFS upgrade_voltage target_voltage=%d \n",target_voltage);
	if (target_voltage > gpu_dfs_ctx.cur_voltage)
	{
		//pmic upgrade voltage
		reg_ret = regulator_set_voltage(gpu_reg,target_voltage,target_voltage);
		KBASE_DEBUG_PRINT(3, "GPU_DVFS upgrade_voltage reg_ret %d voltage=%d\n",reg_ret, regulator_get_voltage(gpu_reg));
		if (reg_ret)
		{
			KBASE_DEBUG_PRINT(2, "GPU_DVFS upgrade_voltage reg_ret %d fail!\n",reg_ret);
			result = false;
		}
		else
		{
			//set gpu current voltage
			gpu_dfs_ctx.cur_voltage = target_voltage;
			gpu_voltage_cur = gpu_dfs_ctx.cur_voltage;
		}
	}

	return (result);
}

static int downgrade_voltage(void)
{
	bool result = true;
	int reg_ret = 0, target_voltage = 0;

	target_voltage = gpu_dfs_ctx.freq_next->volt;
	KBASE_DEBUG_PRINT(3, "GPU_DVFS downgrade_voltage target_voltage=%d \n",target_voltage);
	if (target_voltage < gpu_dfs_ctx.cur_voltage)
	{
		//pmic downgrade voltage
		reg_ret = regulator_set_voltage(gpu_reg,target_voltage,target_voltage);
		KBASE_DEBUG_PRINT(3, "GPU_DVFS downgrade_voltage reg_ret %d voltage=%d\n",reg_ret, regulator_get_voltage(gpu_reg));
		if (reg_ret)
		{
			KBASE_DEBUG_PRINT(2, "GPU_DVFS downgrade_voltage reg_ret %d fail!\n",reg_ret);
			result = false;
		}
		else
		{
			//set gpu current voltage
			gpu_dfs_ctx.cur_voltage = target_voltage;
			gpu_voltage_cur = gpu_dfs_ctx.cur_voltage;
		}
	}

	return (result);
}
#endif

static int freq_search(struct gpu_freq_info freq_list[], int len, int key)
{
	int low=0, high=len-1, mid;

	if (0 > key)
	{
		return -1;
	}

	while(low <= high)
	{
		mid = (low+high)/2;
		if(key == freq_list[mid].freq)
		{
			return mid;
		}

		if(key < freq_list[mid].freq)
		{
			high = mid-1;
		}
		else
		{
			low = mid+1;
		}
	}
	return -1;
}

/*
 * Returns 0 or the clock error. On error freq_cur is kept: the caller must
 * not lower the voltage, the GPU may still run at the old (higher) rate.
 */
static int gpu_change_freq(void)
{
	int err = 0;

	KBASE_DEBUG_PRINT(3, "GPU_DVFS gpu_change_freq cur_freq %6d -> next_freq %6d\n",
				gpu_dfs_ctx.freq_cur->freq, gpu_dfs_ctx.freq_next->freq);
	if(gpu_dfs_ctx.freq_next != gpu_dfs_ctx.freq_cur)
	{
		if(gpu_dfs_ctx.freq_next->clk_src != gpu_dfs_ctx.freq_cur->clk_src)
		{
			err = clk_set_rate(gpu_dfs_ctx.freq_next->clk_src, gpu_dfs_ctx.freq_next->freq*1000);
			if (!err)
				err = clk_set_parent(gpu_dfs_ctx.gpu_clock, gpu_dfs_ctx.freq_next->clk_src);
		}
		else
		{
			if(gpu_dfs_ctx.freq_next->freq != gpu_dfs_ctx.freq_cur->freq)
			{
				err = clk_set_parent(gpu_dfs_ctx.gpu_clock, gpu_dfs_ctx.freq_default->clk_src);
				if (!err)
					err = clk_set_rate(gpu_dfs_ctx.freq_next->clk_src, gpu_dfs_ctx.freq_next->freq*1000);
				if (!err)
					err = clk_set_parent(gpu_dfs_ctx.gpu_clock, gpu_dfs_ctx.freq_next->clk_src);
			}
		}
		if (err) {
			pr_err_ratelimited("mali: GPU clock change %d -> %d kHz failed (%d)\n",
					   gpu_dfs_ctx.freq_cur->freq,
					   gpu_dfs_ctx.freq_next->freq, err);
			return err;
		}
		if(gpu_dfs_ctx.freq_next->div != gpu_dfs_ctx.freq_cur->div)
		{
			mali_set_div(gpu_dfs_ctx.freq_next);
			}

			gpu_dfs_ctx.freq_cur = gpu_dfs_ctx.freq_next;
			gpu_freq_cur = gpu_dfs_ctx.freq_cur->freq;
#ifdef CONFIG_MALI_SPRD_CLOCK_TRACE
			trace_clock_set_rate(VENDOR_FTRACE_MODULE_NAME, gpu_dfs_ctx.freq_cur->freq, raw_smp_processor_id());
#endif
		}

	return 0;
}

static int gpu_change_volt_freq(void)
{
	int result = -1;
#ifdef CONFIG_ADJUST_VOLTAGE
	bool ret = false;

	down(gpu_dfs_ctx.sem);
	KBASE_DEBUG_PRINT(3, "GPU_DVFS gpu_change_volt_freq gpu_power_on=%d, gpu_clock_on=%d\n",
		gpu_dfs_ctx.gpu_power_on, gpu_dfs_ctx.gpu_clock_on);
	//upgrade voltage
	ret = upgrade_voltage();

	if (ret)
	{
		result = 0;
		//set frequency
		if (gpu_dfs_ctx.gpu_power_on && gpu_dfs_ctx.gpu_clock_on)
		{
			result = gpu_change_freq();
		}
		else
		{
			//power on will set freq
			gpu_dfs_ctx.freq_cur = gpu_dfs_ctx.freq_next;
		}

		/* a failed clock change may leave the old, faster rate */
		if (!result)
			ret = downgrade_voltage();
	}
	up(gpu_dfs_ctx.sem);
#else
	down(gpu_dfs_ctx.sem);
	KBASE_DEBUG_PRINT(3, "GPU_DVFS gpu_change_volt_freq gpu_power_on=%d, gpu_clock_on=%d\n",
		gpu_dfs_ctx.gpu_power_on, gpu_dfs_ctx.gpu_clock_on);
	result = 0;
	//set frequency
	if (gpu_dfs_ctx.gpu_power_on && gpu_dfs_ctx.gpu_clock_on)
	{
		result = gpu_change_freq();
	}
	else
	{
		//power on will set freq
		gpu_dfs_ctx.freq_cur = gpu_dfs_ctx.freq_next;
	}

	up(gpu_dfs_ctx.sem);
#endif

	return (result);
}
#endif

#ifdef CONFIG_MALI_MIDGARD_DVFS
static const struct gpu_freq_info* get_next_freq(const struct gpu_freq_info* min_freq, const struct gpu_freq_info* max_freq, int target)
{
	const struct gpu_freq_info* freq;

	for (freq = min_freq; freq <= max_freq; freq++)
	{
		if (freq->up_threshold > target)
		{
			return freq;
		}
	}
	return max_freq;
}

static void gpu_dfs_func(struct work_struct *work)
{
	gpu_change_volt_freq();
}

static DECLARE_WORK(gpu_dfs_work, &gpu_dfs_func);

int kbase_platform_dvfs_event(struct kbase_device *kbdev, u32 utilisation,
	u32 util_gl_share, u32 util_cl_share[2])
{
	int result = 0, min_index = -1, max_index = -1;

	gpu_dfs_ctx.cur_load = utilisation;

	KBASE_DEBUG_PRINT(3, "MALI_DVFS utilisation=%d util_gl_share=%d, util_cl_share[0]=%d, util_cl_share[1]=%d \n",
		utilisation, util_gl_share, util_cl_share[0], util_cl_share[1]);
	KBASE_DEBUG_PRINT(3, "MALI_DVFS gpu_boost_level:%d \n", gpu_boost_level);

	switch(gpu_boost_level)
	{
	case 10:
		gpu_dfs_ctx.freq_max =
		gpu_dfs_ctx.freq_min = &gpu_dfs_ctx.freq_list[gpu_dfs_ctx.freq_list_len-1];
		break;

	case 9:
		gpu_dfs_ctx.freq_max =
		gpu_dfs_ctx.freq_min = gpu_dfs_ctx.freq_9;
		break;

	case 7:
		gpu_dfs_ctx.freq_max =
		gpu_dfs_ctx.freq_min = gpu_dfs_ctx.freq_7;
		break;

	case 5:
		gpu_dfs_ctx.freq_max =
		gpu_dfs_ctx.freq_min = gpu_dfs_ctx.freq_5;
		break;

	case 0:
	default:
		gpu_dfs_ctx.freq_max = gpu_dfs_ctx.freq_range_max;
		gpu_dfs_ctx.freq_min = gpu_dfs_ctx.freq_range_min;
		break;
	}

	gpu_boost_level = 0;

	//limit min freq
	min_index = freq_search(gpu_dfs_ctx.freq_list, gpu_dfs_ctx.freq_list_len, gpu_freq_min_limit);
	if ((0 <= min_index) &&
		(gpu_dfs_ctx.freq_min->freq < gpu_dfs_ctx.freq_list[min_index].freq))
	{
		gpu_dfs_ctx.freq_min = &gpu_dfs_ctx.freq_list[min_index];
		if (gpu_dfs_ctx.freq_min->freq > gpu_dfs_ctx.freq_max->freq)
		{
			gpu_dfs_ctx.freq_max = gpu_dfs_ctx.freq_min;
		}
	}

	//limit max freq
	max_index = freq_search(gpu_dfs_ctx.freq_list, gpu_dfs_ctx.freq_list_len, gpu_freq_max_limit);
	if ((0 <= max_index) &&
		(gpu_dfs_ctx.freq_max->freq > gpu_dfs_ctx.freq_list[max_index].freq))
	{
		gpu_dfs_ctx.freq_max = &gpu_dfs_ctx.freq_list[max_index];
		if (gpu_dfs_ctx.freq_max->freq < gpu_dfs_ctx.freq_min->freq)
		{
			gpu_dfs_ctx.freq_min = gpu_dfs_ctx.freq_max;
		}
	}

	// if the loading ratio is greater then 90%, switch the clock to the maximum
	if(gpu_dfs_ctx.cur_load >= (100*UP_THRESHOLD))
	{
		gpu_dfs_ctx.freq_next = gpu_dfs_ctx.freq_max;
	}
	else
	{
		int target_freq = gpu_dfs_ctx.freq_cur->freq * gpu_dfs_ctx.cur_load / 100;
		gpu_dfs_ctx.freq_next = get_next_freq(gpu_dfs_ctx.freq_min, gpu_dfs_ctx.freq_max, target_freq);
	}

	KBASE_DEBUG_PRINT(3, "MALI_DVFS util %3d; cur_freq %6d -> next_freq %6d\n",
		gpu_dfs_ctx.cur_load, gpu_dfs_ctx.freq_cur->freq, gpu_dfs_ctx.freq_next->freq);

	if(gpu_dfs_ctx.freq_next->freq != gpu_dfs_ctx.freq_cur->freq)
	{
		queue_work(gpu_dfs_ctx.gpu_dfs_workqueue, &gpu_dfs_work);
	}

	return (result);
}
#endif/*CONFIG_MALI_MIDGARD_DVFS*/

#ifdef CONFIG_MALI_DEVFREQ
#define FREQ_KHZ    1000

int kbase_platform_get_init_freq(void)
{
	return (gpu_dfs_ctx.freq_default->freq * FREQ_KHZ);
}

/*
 * Highest frequency the board allows, from the DT "sprd,dfs-range-max" index
 * (Hz). The stock driver only ever asked for frequencies inside that range;
 * the devfreq governor clamps its requests to it (kbase_devfreq_target()).
 */
unsigned long kbase_platform_max_freq(void)
{
	if (!gpu_dfs_ctx.freq_range_max)
		return 0;
	return (unsigned long)gpu_dfs_ctx.freq_range_max->freq * FREQ_KHZ;
}

int kbase_platform_set_freq(int freq)
{
	int result = -1, index = -1;

	freq = freq/FREQ_KHZ;
	index = freq_search(gpu_dfs_ctx.freq_list, gpu_dfs_ctx.freq_list_len, freq);
	KBASE_DEBUG_PRINT(2, "GPU_DVFS  kbase_platform_set_freq index=%d! freq=%d \n", index, freq);
	if (0 <= index)
	{
		gpu_dfs_ctx.freq_next = &gpu_dfs_ctx.freq_list[index];
		result = gpu_change_volt_freq();
	}

	return (result);
}

#ifdef CONFIG_MALI_BOOST
void kbase_platform_modify_target_freq(struct device *dev, unsigned long *target_freq)
{
	int min_index = -1, max_index = -1;
	struct kbase_device *kbdev = dev_get_drvdata(dev);

	switch(gpu_boost_level)
	{
	case 10:
		gpu_dfs_ctx.freq_max =
		gpu_dfs_ctx.freq_min = &gpu_dfs_ctx.freq_list[gpu_dfs_ctx.freq_list_len-1];
		break;

	case 9:
		gpu_dfs_ctx.freq_max =
		gpu_dfs_ctx.freq_min = gpu_dfs_ctx.freq_9;
		break;

	case 7:
		gpu_dfs_ctx.freq_max =
		gpu_dfs_ctx.freq_min = gpu_dfs_ctx.freq_7;
		break;

	case 5:
		gpu_dfs_ctx.freq_max =
		gpu_dfs_ctx.freq_min = gpu_dfs_ctx.freq_5;
		break;

	case 0:
	default:
		gpu_dfs_ctx.freq_max = gpu_dfs_ctx.freq_range_max;
		gpu_dfs_ctx.freq_min = gpu_dfs_ctx.freq_range_min;
		break;
	}

	//limit min freq
	min_index = freq_search(gpu_dfs_ctx.freq_list, gpu_dfs_ctx.freq_list_len, kbdev->devfreq->min_freq/FREQ_KHZ);
	if ((0 <= min_index) &&
		(gpu_dfs_ctx.freq_min->freq < gpu_dfs_ctx.freq_list[min_index].freq))
	{
		gpu_dfs_ctx.freq_min = &gpu_dfs_ctx.freq_list[min_index];
		if (gpu_dfs_ctx.freq_min->freq > gpu_dfs_ctx.freq_max->freq)
		{
			gpu_dfs_ctx.freq_max = gpu_dfs_ctx.freq_min;
		}
	}

	//limit max freq
	max_index = freq_search(gpu_dfs_ctx.freq_list, gpu_dfs_ctx.freq_list_len, kbdev->devfreq->max_freq/FREQ_KHZ);
	if ((0 <= max_index) &&
		(gpu_dfs_ctx.freq_max->freq > gpu_dfs_ctx.freq_list[max_index].freq))
	{
		gpu_dfs_ctx.freq_max = &gpu_dfs_ctx.freq_list[max_index];
		if (gpu_dfs_ctx.freq_max->freq < gpu_dfs_ctx.freq_min->freq)
		{
			gpu_dfs_ctx.freq_min = gpu_dfs_ctx.freq_max;
		}
	}

	KBASE_DEBUG_PRINT(3, "GPU_DVFS kbase_platform_modify_target_freq gpu_boost_level:%d min_freq=%d max_freq=%d target_freq=%lu\n",
		gpu_boost_level, gpu_dfs_ctx.freq_min->freq, gpu_dfs_ctx.freq_max->freq, *target_freq);

	gpu_boost_level = 0;

	//set target frequency
	if (*target_freq < gpu_dfs_ctx.freq_min->freq*FREQ_KHZ)
	{
		*target_freq = gpu_dfs_ctx.freq_min->freq*FREQ_KHZ;
	}
	if (*target_freq > gpu_dfs_ctx.freq_max->freq*FREQ_KHZ)
	{
		*target_freq = gpu_dfs_ctx.freq_max->freq*FREQ_KHZ;
	}
}
#endif
#endif

bool kbase_mali_is_powered(void)
{
	bool result = false;

	down(gpu_dfs_ctx.sem);
	if (gpu_dfs_ctx.gpu_power_on && gpu_dfs_ctx.gpu_clock_on)
	{
		result = true;
	}
	up(gpu_dfs_ctx.sem);

	return (result);
}

#ifdef CONFIG_MALI_BOOST
void kbase_platform_set_boost(struct kbase_device *kbdev, int boost_level)
{
	if (gpu_boost_level < boost_level)
	{
		gpu_boost_level = boost_level;
		KBASE_DEBUG_PRINT(3, "kbase_platform_set_boost gpu_boost_level =%d\n", gpu_boost_level);
	}
}
#endif
