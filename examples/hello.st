# Minimal introduction to Strata types, functions, and loops
import stdio.h
import stdint.h
import stdbool.h

MAX_GREETINGS = 3

def greet(name: cstr, count: i32) -> i32:
    if name == nil or count <= 0:
        return 0
    for i in range(count):
        printf("[%d] Hello from Strata, %s!\n", i + 1, name)
    return count

def main() -> i32:
    sent: i32 = greet("World", MAX_GREETINGS)
    if sent > 0 and not (sent > 10):
        printf("Successfully sent %d greetings.\n", sent)
    return 0
