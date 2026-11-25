# Refactoring

- Refactors must add zero production runtime cost. Do not trade CPU work, allocations, memory, I/O, or startup loading for cleaner structure.
- Check optimized AMD64, ARM64, and ARMv6 code plus browser allocations and production assets when those paths change.
- Correctness tests alone do not establish runtime neutrality. State any unverified performance impact explicitly.
