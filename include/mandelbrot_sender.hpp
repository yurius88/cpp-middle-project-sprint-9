#pragma once

#include "mandelbrot_fractal_utils.hpp"
#include "types_sfml.hpp"
#include <print>

#include <stdexec/execution.hpp>

using namespace std::chrono_literals;
namespace ex = stdexec;

namespace mandelbrot {

static auto MakeComputeSender(RenderSettings settings, ViewPort viewport) {
    static AvrTimeCounter time_counter;
    return ex::then([](FrameBuffer *fb) {
               time_counter.Start();
               return fb;
           }) |
           ex::bulk(ex::par, settings.height,
                    [settings, viewport](std::uint32_t y, FrameBuffer *fb) {
                        for (std::uint32_t x = 0; x < fb->width; ++x) {
                            const Complex c = Pixel2DToComplex(x, y, viewport, fb->width, fb->height);
                            const std::uint32_t iterations =
                                CalculateIterationsForPoint(c, settings.max_iterations, settings.escape_radius);
                            const RgbColor color = IterationsToColor(iterations, settings.max_iterations);

                            const std::size_t idx = (static_cast<std::size_t>(y) * fb->width + x) * 4u;
                            fb->rgba[idx + 0] = color.r;
                            fb->rgba[idx + 1] = color.g;
                            fb->rgba[idx + 2] = color.b;
                            fb->rgba[idx + 3] = 255;
                        }
                    }) |
           ex::then([](FrameBuffer *fb) {
               time_counter.End();
               if (time_counter.Count() % 10 == 0) {
                   std::println("\nAverage compute time: {} ms over {} frames", time_counter.GetAvr(),
                                time_counter.Count());
               }
               return fb;
           });
}

}  // namespace mandelbrot
