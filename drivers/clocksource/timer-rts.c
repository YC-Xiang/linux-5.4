#include <linux/init.h>
#include <linux/err.h>
#include <linux/time.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/timex.h>

#define RTS_TIMER_EN		0x00
#define RTS_TIMER_COMPARE	0x04
#define RTS_TIMER_CURRENT	0x08
#define RTS_TIMER_MODE		0x0C
#define RTS_TIMER_INT_EN	0x10
#define RTS_TIMER_INT_STS	0x14

#define RTS_TIMER_FREQUENCY	25000000

struct rts_timer_data {
	void __iomem *addr;
	int irq;
};

static struct rts_timer_data *rts_tdata;

static unsigned int rts_timer_read(struct rts_timer_data *tdata,
				    unsigned int reg)
{
	return readl(tdata->addr + reg);
}

static int rts_timer_write(struct rts_timer_data *tdata, unsigned int reg,
			    unsigned int value)
{
	writel(value, tdata->addr + reg);

	return 0;
}

static u64 rts_timer_read_count(struct clocksource *cs)
{
	return rts_timer_read(rts_tdata, RTS_TIMER_CURRENT);
}

inline cycles_t rts_get_cycles(void)
{
	return rts_timer_read(rts_tdata, RTS_TIMER_CURRENT);
}
EXPORT_SYMBOL(rts_get_cycles);

static u64 notrace rts_riscv_sched_clock(void)
{
	return rts_timer_read(rts_tdata, RTS_TIMER_CURRENT);
}

static struct clocksource clocksource_rts = {
	.name = "RTS CLOCKSOURCE",
	.read = rts_timer_read_count,
	.mask = CLOCKSOURCE_MASK(32),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
	.rating = 390,
};

static void rts_setup_clocksource(void)
{
	/* enable timer */
	rts_timer_write(rts_tdata, RTS_TIMER_EN, 0x1);

	clocksource_register_hz(&clocksource_rts, RTS_TIMER_FREQUENCY);

	sched_clock_register(rts_riscv_sched_clock, 32, RTS_TIMER_FREQUENCY);
}

static int rts_timer_next_event(unsigned long delta,
		struct clock_event_device *evt)
{
	unsigned int cnt;

	cnt = rts_timer_read(rts_tdata, RTS_TIMER_CURRENT);
	cnt += delta;
	rts_timer_write(rts_tdata, RTS_TIMER_COMPARE, cnt);

	return 0;
}

static struct clock_event_device clockevent_rts = {
	.name = "RTS CLOCKEVENT",
	.rating = 390,
	.features = CLOCK_EVT_FEAT_ONESHOT,
	.set_next_event = rts_timer_next_event,
};

void rts_timer_disable(void)
{
	/* disable xb2 timer */
	rts_timer_write(rts_tdata, RTS_TIMER_EN, 0x0);
}

void rts_timer_enable(void)
{
	/* enable xb2 timer */
	rts_timer_write(rts_tdata, RTS_TIMER_EN, 0x1);
}

static irqreturn_t rts_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = (struct clock_event_device *)dev_id;
	u32 int_val;

	int_val = rts_timer_read(rts_tdata, RTS_TIMER_INT_STS);
	if (int_val & 0x1) {
		rts_timer_write(rts_tdata, RTS_TIMER_INT_STS, 0x1);
		evt->event_handler(evt);
	}

	return IRQ_HANDLED;
}

static void rts_setup_clockevent(void)
{
	int ret;

	ret = request_irq(rts_tdata->irq, rts_timer_interrupt,
				IRQF_TIMER | IRQF_IRQPOLL | IRQF_NOBALANCING,
					"rts timer", (void *)&clockevent_rts);

	if (ret)
		pr_err("requset timer irq failed\n");

	/* enable timer interrupt */
	rts_timer_write(rts_tdata, RTS_TIMER_INT_STS, 0x1);
	rts_timer_write(rts_tdata, RTS_TIMER_INT_EN, 0x1);

	clockevents_config_and_register(&clockevent_rts, RTS_TIMER_FREQUENCY,
					100, 0x7fffffff);
}

static int __init rts_timer_init(struct device_node *node)
{
	struct rts_timer_data *data;
	void __iomem *timer_base;
	int irq;

	timer_base = of_io_request_and_map(node, 0, of_node_full_name(node));
	if (IS_ERR(timer_base)) {
		pr_err("Can't map registers");
		return PTR_ERR(timer_base);
	}

	irq = irq_of_parse_and_map(node, 0);
	if (irq <= 0) {
		pr_err("Can't parse IRQ");
		return -EINVAL;
	}

	data = kzalloc(sizeof(struct rts_timer_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->addr = timer_base;
	data->irq = irq;

	rts_tdata = data;

	rts_setup_clocksource();
	rts_setup_clockevent();

	return 0;
}
TIMER_OF_DECLARE(rts_timer, "realtek,rts3923-timer", rts_timer_init);
