# Structs, enums, pointers, and switch/case in Strata
import stdio.h
import stdint.h
import stdbool.h
import math.h

enum ShapeKind:
    CIRCLE = 0
    RECTANGLE = 1

struct Vec2:
    x: f64
    y: f64

struct Shape:
    kind: ShapeKind
    origin: Vec2
    width: f64
    height: f64

def area(s: addr[const Shape]) -> f64:
    if s == nil:
        return 0.0
    switch s.kind:
        case CIRCLE:
            return 3.141592653589793 * s.width * s.width
        case RECTANGLE:
            return s.width * s.height
        default:
            return 0.0

def main() -> i32:
    c: Shape
    c.kind = CIRCLE
    c.origin.x = 0.0
    c.origin.y = 0.0
    c.width = 5.0
    c.height = 5.0
    printf("Circle area (r=%.1f): %.4f\n", c.width, area(c.addr))
    return 0
