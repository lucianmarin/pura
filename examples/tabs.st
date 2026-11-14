# Tab-indented Strata demonstrating auto-detected \t indentation
import stdio.h
import stdint.h

def fib(n: i32) -> i64:
	if n <= 1:
		return n
	a: i64 = 0
	b: i64 = 1
	for i in range(2, n + 1):
		next: i64 = a + b
		a = b
		b = next
	return b

def main() -> i32:
	for k in range(10):
		printf("fib(%d) = %lld\n", k, (long long)fib(k))
	return 0
