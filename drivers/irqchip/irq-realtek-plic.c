// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019-2020 Realtek Semiconductor Corp.	All rights reserved.
 *
 * This program is free software; you can redistribute	it and/or modify it
 * under  the terms of	the GNU General	 Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
#define pr_fmt(fmt) "plic: " fmt
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <asm/smp.h>

/*
 * Each interrupt source has a priority register associated with it.
 * We always hardwire it to one in Linux.
 */
#define PRIORITY_BASE			0x800
#define     PRIORITY_PER_ID		4

/*
 * Each hart context has a vector of interrupt enable bits associated with it.
 * There's one bit for each interrupt source.
 */
#define ENABLE_BASE			0x60
#define     ENABLE_PER_HART		0x100

/*
 * Each hart context has a set of control registers associated with it.  Right
 * now there's only two: a source priority threshold over which the hart will
 * take an interrupt, and a register to claim interrupts.
 */
#define CONTEXT_BASE			0x0
#define     CONTEXT_PER_HART		0x100
#define     CONTEXT_THRESHOLD		0x5c
#define     CONTEXT_CLAIM		0x54
#define     CONTEXT_COMPLETE		0x58

#define	PLIC_DISABLE_THRESHOLD		0x7
#define	PLIC_ENABLE_THRESHOLD		0

struct plic_priv {
	struct cpumask lmask;
	struct irq_domain *irqdomain;
	void __iomem *regs;
};

struct plic_handler {
	bool			present;
	void __iomem		*hart_base;
	/*
	 * Protect mask operations on the registers given that we can't
	 * assume atomic memory operations work on them.
	 */
	raw_spinlock_t		enable_lock;
	void __iomem		*enable_base;
	struct plic_priv	*priv;
};
static int plic_parent_irq;
static bool plic_cpuhp_setup_done;
static DEFINE_PER_CPU(struct plic_handler, plic_handlers);

static inline void plic_toggle(struct plic_handler *handler,
				int hwirq, int enable)
{
	u32 __iomem *reg = handler->enable_base + (hwirq / 32) * sizeof(u32);
	u32 hwirq_mask = 1 << (hwirq % 32);

	raw_spin_lock(&handler->enable_lock);
	if (enable)
		writel(readl(reg) | hwirq_mask, reg);
	else
		writel(readl(reg) & ~hwirq_mask, reg);
	raw_spin_unlock(&handler->enable_lock);
}

static inline void plic_irq_toggle(const struct cpumask *mask,
				   struct irq_data *d, int enable)
{
	int cpu;
	struct plic_priv *priv = irq_data_get_irq_chip_data(d);

	writel(enable, priv->regs + PRIORITY_BASE + d->hwirq * PRIORITY_PER_ID);
	for_each_cpu(cpu, mask) {
		struct plic_handler *handler = per_cpu_ptr(&plic_handlers, cpu);

		if (handler->present &&
		    cpumask_test_cpu(cpu, &handler->priv->lmask))
			plic_toggle(handler, d->hwirq, enable);
	}
}

static void plic_irq_unmask(struct irq_data *d)
{
	struct cpumask amask;
	unsigned int cpu;
	struct plic_priv *priv = irq_data_get_irq_chip_data(d);

	cpumask_and(&amask, &priv->lmask, cpu_online_mask);
	cpu = cpumask_any_and(irq_data_get_affinity_mask(d),
					   &amask);
	if (WARN_ON_ONCE(cpu >= nr_cpu_ids))
		return;
	plic_irq_toggle(cpumask_of(cpu), d, 1);
}

static void plic_irq_mask(struct irq_data *d)
{
	struct plic_priv *priv = irq_data_get_irq_chip_data(d);

	plic_irq_toggle(&priv->lmask, d, 0);
}

#ifdef CONFIG_SMP
static int plic_set_affinity(struct irq_data *d,
			     const struct cpumask *mask_val, bool force)
{
	unsigned int cpu;
	struct cpumask amask;
	struct plic_priv *priv = irq_data_get_irq_chip_data(d);

	cpumask_and(&amask, &priv->lmask, mask_val);

	if (force)
		cpu = cpumask_first(&amask);
	else
		cpu = cpumask_any_and(&amask, cpu_online_mask);

	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	plic_irq_toggle(&priv->lmask, d, 0);
	plic_irq_toggle(cpumask_of(cpu), d, !irqd_irq_masked(d));

	irq_data_update_effective_affinity(d, cpumask_of(cpu));

	return IRQ_SET_MASK_OK_DONE;
}
#endif

static void plic_irq_eoi(struct irq_data *d) /// ???eoi:end of interrupt?
{
	struct plic_handler *handler = this_cpu_ptr(&plic_handlers);

	if (irqd_irq_masked(d)) {
		plic_irq_unmask(d);
		writel(d->hwirq, handler->hart_base + CONTEXT_COMPLETE);
		plic_irq_mask(d);
	} else {
		writel(d->hwirq, handler->hart_base + CONTEXT_COMPLETE);
	}
}

static struct irq_chip plic_chip = {
	.name		= "Realtek PLIC",
	.irq_mask	= plic_irq_mask,
	.irq_unmask	= plic_irq_unmask,
	.irq_eoi	= plic_irq_eoi, /// 让CPU可以通知interrupt controller，它已经处理完一个中断
#ifdef CONFIG_SMP
	.irq_set_affinity = plic_set_affinity,
#endif
};

static int plic_irqdomain_map(struct irq_domain *d, unsigned int irq,
			      irq_hw_number_t hwirq)
{
	struct plic_priv *priv = d->host_data;
	/// 这里用irq_domain_set_info，调用到__irq_set_handler(virq, handler, 0, handler_name);
	/// 注意到is_chained为0，但我们是第二级的interrupt controller.
	irq_domain_set_info(d, irq, hwirq, &plic_chip, d->host_data,
			    handle_fasteoi_irq, NULL, NULL);
	irq_set_noprobe(irq);
	irq_set_affinity(irq, &priv->lmask);
	return 0;
}

static int plic_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs, void *arg)
{
	int i, ret;
	irq_hw_number_t hwirq;
	unsigned int type;
	struct irq_fwspec *fwspec = arg;

	ret = irq_domain_translate_onecell(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	for (i = 0; i < nr_irqs; i++) {
		ret = plic_irqdomain_map(domain, virq + i, hwirq + i);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct irq_domain_ops plic_irqdomain_ops = {
	.translate	= irq_domain_translate_onecell,
	.alloc		= plic_irq_domain_alloc,
	.free		= irq_domain_free_irqs_top,
};

/*
 * Handling an interrupt is a two-step process: first you claim the interrupt
 * by reading the claim register, then you complete the interrupt by writing
 * that source ID back to the same claim register.  This automatically enables
 * and disables the interrupt, so there's nothing else to do.
 */
static void plic_handle_irq(struct irq_desc *desc)
{
	struct plic_handler *handler = this_cpu_ptr(&plic_handlers);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	void __iomem *claim = handler->hart_base + CONTEXT_CLAIM;
	irq_hw_number_t hwirq;

	WARN_ON_ONCE(!handler->present);

	chained_irq_enter(chip, desc); /// 级联的第二级中断处理函数需要

	while ((hwirq = readl(claim))) {
		int irq = irq_find_mapping(handler->priv->irqdomain, hwirq);

		if (unlikely(irq <= 0))
			pr_warn_ratelimited("can't find mapping for hwirq %lu\n",
					hwirq);
		else
			generic_handle_irq(irq);
	}

	chained_irq_exit(chip, desc); /// 级联的第二级中断处理函数需要
}

static void plic_set_threshold(struct plic_handler *handler, u32 threshold)
{
	/* priority must be > threshold to trigger an interrupt */
	writel(threshold, handler->hart_base + CONTEXT_THRESHOLD);
}

static int plic_dying_cpu(unsigned int cpu)
{
	if (plic_parent_irq)
		disable_percpu_irq(plic_parent_irq);

	return 0;
}

static int plic_starting_cpu(unsigned int cpu)
{
	struct plic_handler *handler = this_cpu_ptr(&plic_handlers);

	if (plic_parent_irq)
		enable_percpu_irq(plic_parent_irq,
				  irq_get_trigger_type(plic_parent_irq));
	else
		pr_warn("cpu%d: parent irq not available\n", cpu);
	plic_set_threshold(handler, PLIC_ENABLE_THRESHOLD);

	return 0;
}

static int __init plic_init(struct device_node *node,
		struct device_node *parent)
{
	int error = 0, nr_contexts, nr_handlers = 0, i;
	u32 nr_irqs;
	struct plic_priv *priv;
	struct plic_handler *handler;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regs = of_iomap(node, 0);
	if (WARN_ON(!priv->regs)) {
		error = -EIO;
		goto out_free_priv;
	}

	error = -EINVAL;
	of_property_read_u32(node, "riscv,ndev", &nr_irqs); /// 128
	if (WARN_ON(!nr_irqs))
		goto out_iounmap;

	nr_contexts = of_irq_count(node); /// nr_contexts = 1?
	if (WARN_ON(!nr_contexts))
		goto out_iounmap;

	error = -ENOMEM;
	priv->irqdomain = irq_domain_add_linear(node, nr_irqs + 1,
			&plic_irqdomain_ops, priv);
	if (WARN_ON(!priv->irqdomain))
		goto out_iounmap;

	for (i = 0; i < nr_contexts; i++) {
		struct of_phandle_args parent;
		irq_hw_number_t hwirq;
		int cpu, hartid;

		if (of_irq_parse_one(node, i, &parent)) { /// interrupts-extended = <&cpu_intc 9>; 这个phandle保存进parent
			pr_err("failed to parse parent for context %d.\n", i);
			continue;
		}

		/*
		 * Skip contexts other than external interrupts for our
		 * privilege level.
		 */
		if (parent.args[0] != RV_IRQ_EXT) /// RV_IRQ_EXT = 9
			continue;

		hartid = riscv_of_parent_hartid(parent.np);
		if (hartid < 0) {
			pr_warn("failed to parse hart ID for context %d.\n", i);
			continue;
		}

		cpu = riscv_hartid_to_cpuid(hartid);
		if (cpu < 0) {
			pr_warn("Invalid cpuid for context %d\n", i);
			continue;
		}

		/* Find parent domain and register chained handler */
		if (!plic_parent_irq && irq_find_host(parent.np)) {
			plic_parent_irq = irq_of_parse_and_map(node, i); /// 映射hw id(9)到irq number。plic_parent_irq是返回的irq number
			if (plic_parent_irq)
				irq_set_chained_handler(plic_parent_irq,
							plic_handle_irq); /// 设定desc->handler.__irq_set_handler(irq, handle, 1, NULL);cpu intc 发生plic_parent_irq 9号中断，会进入plic_handle_irq
		}

		/*
		 * When running in M-mode we need to ignore the S-mode handler.
		 * Here we assume it always comes later, but that might be a
		 * little fragile.
		 */
		handler = per_cpu_ptr(&plic_handlers, cpu);
		if (handler->present) {
			pr_warn("handler already present for context %d.\n", i);
			plic_set_threshold(handler, PLIC_DISABLE_THRESHOLD);
			goto done;
		}

		cpumask_set_cpu(cpu, &priv->lmask);
		handler->present = true;
		handler->hart_base =
			priv->regs + CONTEXT_BASE + i * CONTEXT_PER_HART;
		raw_spin_lock_init(&handler->enable_lock);
		handler->enable_base =
			priv->regs + ENABLE_BASE + i * ENABLE_PER_HART;
		handler->priv = priv;
done:
		for (hwirq = 1; hwirq <= nr_irqs; hwirq++)
			plic_toggle(handler, hwirq, 0);
		nr_handlers++;
	}

	/*
	 * We can have multiple PLIC instances so setup cpuhp state only
	 * when context handler for current/boot CPU is present.
	 */
	handler = this_cpu_ptr(&plic_handlers);
	if (handler->present && !plic_cpuhp_setup_done) {
		cpuhp_setup_state(CPUHP_AP_IRQ_REALTEK_PLIC_STARTING,
				  "irqchip/realtek/plic:starting",
				  plic_starting_cpu, plic_dying_cpu);
		plic_cpuhp_setup_done = true;
	}

	pr_info("%pOFP: mapped %d interrupts with %d handlers for"
		" %d contexts.\n", node, nr_irqs, nr_handlers, nr_contexts);
	return 0;

out_iounmap:
	iounmap(priv->regs);
out_free_priv:
	kfree(priv);
	return error;
}

IRQCHIP_DECLARE(realtek_plic, "realtek,plic-1.0.0", plic_init);
IRQCHIP_DECLARE(riscv_plic0, "riscv,plic0", plic_init); /* for legacy systems */
