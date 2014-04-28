#include <Halide.h>
#include <stdio.h>

using namespace Halide;

// Test that internal allocations work correctly with copy_to_dev. This
// requires that suitable buffer_t objects are created internally.
int main() {
    Image<uint8_t> input(255, 10, 3);
    for (int y=0; y<input.height(); y++) {
        for (int x=0; x<input.width(); x++) {
            for (int c=0; c<3; c++) {
              input(x, y, c) = 10*x + y + c;
            }
        }
    }

    Var x, y, c;
    Func g, h;
    h(x, y, c) = input(x, y, c);
    h.compute_root();  // force internal allocation of h

    // access h from shader to trigger copy_to_dev operation
    g(x, y, c) = h(x, y, c);
    g.bound(c, 0, 3);
    g.glsl(x, y, c);

    Image<uint8_t> out(255, 10, 3);
    g.realize(out);
    out.copy_to_host();

    for (int y=0; y<out.height(); y++) {
        for (int x=0; x<out.width(); x++) {
            if (!(out(x, y, 0) == input(x, y, 0) &&
                  out(x, y, 1) == input(x, y, 1) &&
                  out(x, y, 2) == input(x, y, 2))) {
                fprintf(stderr, "Incorrect pixel (%d,%d,%d) != (%d,%d,%d) at x=%d y=%d.\n",
                        out(x, y, 0), out(x, y, 1), out(x, y, 2),
                        input(x, y, 0), input(x, y, 1), input(x, y, 2),
                        x, y);
                return 1;
            }
        }
    }
    return 0;
}