/* This single-CPU port uses the CPU timer and bounded polling for devices. */
void plicinit(void) { }
void plicinithart(void) { }
int plic_claim(void) { return 0; }
void plic_complete(int irq) { (void)irq; }
