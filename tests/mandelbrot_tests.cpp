#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <thread>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "mandelbrot_fractal_utils.hpp"
#include "mandelbrot_sender.hpp"
#include "sfml_events_handler.hpp"
#include "types_core.hpp"
#include "types_sfml.hpp"

using namespace mandelbrot;

namespace {

bool SameColor(const RgbColor &a, const RgbColor &b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

RgbColor PixelAt(const FrameBuffer &fb, std::uint32_t x, std::uint32_t y) {
    const std::size_t idx = (static_cast<std::size_t>(y) * fb.width + x) * 4u;
    return RgbColor{fb.rgba[idx], fb.rgba[idx + 1], fb.rgba[idx + 2]};
}

}  // namespace

TEST(CalculateIterationsForPoint, PointsInsideSetReachMaxIterations) {
    EXPECT_EQ(CalculateIterationsForPoint({0.0, 0.0}, 100, 2.0), 100u);
    EXPECT_EQ(CalculateIterationsForPoint({-1.0, 0.0}, 100, 2.0), 100u);
    EXPECT_EQ(CalculateIterationsForPoint({-0.5, 0.0}, 100, 2.0), 100u);
}

TEST(CalculateIterationsForPoint, PointsOutsideSetEscapeEarly) {
    EXPECT_EQ(CalculateIterationsForPoint({3.0, 0.0}, 100, 2.0), 1u);
    EXPECT_EQ(CalculateIterationsForPoint({2.0, 0.0}, 100, 2.0), 2u);
}

TEST(CalculateIterationsForPoint, IsConstexpr) {
    static_assert(CalculateIterationsForPoint({0.0, 0.0}, 10, 2.0) == 10u);
    static_assert(CalculateIterationsForPoint({3.0, 0.0}, 10, 2.0) == 1u);
}

TEST(Pixel2DToComplex, MapsCornersAndCenter) {
    const ViewPort vp = AppState::INITIAL_VIEWPORT;

    const Complex top_left = Pixel2DToComplex(0, 0, vp, 800, 600);
    EXPECT_DOUBLE_EQ(top_left.real(), vp.x_min);
    EXPECT_DOUBLE_EQ(top_left.imag(), vp.y_min);

    const Complex center = Pixel2DToComplex(400, 300, vp, 800, 600);
    EXPECT_DOUBLE_EQ(center.real(), -0.5);
    EXPECT_DOUBLE_EQ(center.imag(), 0.0);

    const Complex bottom_right = Pixel2DToComplex(800, 600, vp, 800, 600);
    EXPECT_DOUBLE_EQ(bottom_right.real(), vp.x_max);
    EXPECT_DOUBLE_EQ(bottom_right.imag(), vp.y_max);
}

TEST(IterationsToColor, MaxIterationsIsBlack) { EXPECT_TRUE(SameColor(IterationsToColor(100, 100), RgbColors::BLACK)); }

TEST(IterationsToColor, HueMapsToPrimaryColors) {
    EXPECT_TRUE(SameColor(IterationsToColor(0, 3), RgbColor{255, 0, 0}));  // hue 0
    EXPECT_TRUE(SameColor(IterationsToColor(1, 3), RgbColor{0, 255, 0}));  // hue 120
    EXPECT_TRUE(SameColor(IterationsToColor(2, 3), RgbColor{0, 0, 255}));  // hue 240
}

TEST(FrameClock, MeasuresElapsedTimeAndResets) {
    FrameClock clock;
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    EXPECT_GE(clock.GetFrameTime(), std::chrono::milliseconds{20});

    clock.Reset();
    EXPECT_LT(clock.GetFrameTime(), std::chrono::milliseconds{20});
}

TEST(AvrTimeCounter, CountsAndAverages) {
    AvrTimeCounter counter;
    EXPECT_EQ(counter.Count(), 0);
    EXPECT_EQ(counter.GetAvr(), 0);

    for (int i = 0; i < 2; ++i) {
        counter.Start();
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        EXPECT_GE(counter.End(), 10);
    }
    EXPECT_EQ(counter.Count(), 2);
    EXPECT_GE(counter.GetAvr(), 10);

    counter.Reset();
    EXPECT_EQ(counter.Count(), 0);
    EXPECT_EQ(counter.GetAvr(), 0);
}

class ComputeSenderTest : public ::testing::Test {
protected:
    RenderSettings settings{.width = 40, .height = 30, .max_iterations = 50, .escape_radius = 2.0};
    ViewPort viewport = AppState::INITIAL_VIEWPORT;
    FrameBuffer fb = FrameBuffer::Make(settings.width, settings.height);

    void ExpectMatchesReference() const {
        for (std::uint32_t y = 0; y < fb.height; ++y) {
            for (std::uint32_t x = 0; x < fb.width; ++x) {
                const Complex c = Pixel2DToComplex(x, y, viewport, fb.width, fb.height);
                const RgbColor expected =
                    IterationsToColor(CalculateIterationsForPoint(c, settings.max_iterations, settings.escape_radius),
                                      settings.max_iterations);
                ASSERT_TRUE(SameColor(PixelAt(fb, x, y), expected)) << "pixel (" << x << ", " << y << ")";
                ASSERT_EQ(fb.rgba[(static_cast<std::size_t>(y) * fb.width + x) * 4u + 3], 255);
            }
        }
    }
};

TEST_F(ComputeSenderTest, ReturnsSameFrameBuffer) {
    auto result = ex::sync_wait(ex::just(&fb) | MakeComputeSender(settings, viewport));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<0>(*result), &fb);
}

TEST_F(ComputeSenderTest, FillsEveryPixelLikeReference) {
    ex::sync_wait(ex::just(&fb) | MakeComputeSender(settings, viewport));
    ExpectMatchesReference();
}

TEST_F(ComputeSenderTest, CenterOfInitialViewIsInsideSet) {
    ex::sync_wait(ex::just(&fb) | MakeComputeSender(settings, viewport));
    // (20, 15) -> -0.5 + 0i
    EXPECT_TRUE(SameColor(PixelAt(fb, 20, 15), RgbColors::BLACK));
    // (0, 0) -> -2.5 - 2i, вне множества
    EXPECT_FALSE(SameColor(PixelAt(fb, 0, 0), RgbColors::BLACK));
}

TEST_F(ComputeSenderTest, WorksOnThreadPool) {
    exec::static_thread_pool pool{4};
    auto result =
        ex::sync_wait(ex::starts_on(pool.get_scheduler(), ex::just(&fb) | MakeComputeSender(settings, viewport)));
    ASSERT_TRUE(result.has_value());
    ExpectMatchesReference();
}

class SfmlEventHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char *display = std::getenv("DISPLAY");
        if (display == nullptr || *display == '\0') {
            GTEST_SKIP() << "DISPLAY не задан, окно SFML создать нельзя";
        }
        window_.create(sf::VideoMode{settings_.width, settings_.height}, "test", sf::Style::None);
        window_.setVisible(false);
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
    }

    bool RunHandler() { return ex::sync_wait(SfmlEventHandler{window_, settings_, state_}).has_value(); }

    RenderSettings settings_{.width = 80, .height = 60, .max_iterations = 50, .escape_radius = 2.0};
    AppState state_;
    sf::RenderWindow window_;
};

TEST_F(SfmlEventHandlerTest, NoInputLeavesViewportUnchanged) {
    state_.need_rerender = false;
    ASSERT_TRUE(RunHandler());
    EXPECT_FALSE(state_.need_rerender);
    EXPECT_FALSE(state_.should_exit);
    EXPECT_DOUBLE_EQ(state_.viewport.x_min, AppState::INITIAL_VIEWPORT.x_min);
    EXPECT_DOUBLE_EQ(state_.viewport.width(), AppState::INITIAL_VIEWPORT.width());
}

TEST_F(SfmlEventHandlerTest, AutoZoomZoomsTowardsSeahorseValley) {
    state_.need_rerender = false;
    state_.auto_zoom_enabled = true;
    const double old_width = state_.viewport.width();

    ASSERT_TRUE(RunHandler());

    EXPECT_TRUE(state_.need_rerender);
    EXPECT_NEAR(state_.viewport.width(), old_width * 0.8, 1e-12);
    EXPECT_LT(state_.viewport.x_min, AppState::AUTO_ZOOM_TARGET_X);
    EXPECT_GT(state_.viewport.x_max, AppState::AUTO_ZOOM_TARGET_X);
    EXPECT_LT(state_.viewport.y_min, AppState::AUTO_ZOOM_TARGET_Y);
    EXPECT_GT(state_.viewport.y_max, AppState::AUTO_ZOOM_TARGET_Y);
}

TEST_F(SfmlEventHandlerTest, ZoomIsThrottledByInterval) {
    state_.auto_zoom_enabled = true;
    ASSERT_TRUE(RunHandler());
    const double width_after_first = state_.viewport.width();

    ASSERT_TRUE(RunHandler());
    EXPECT_DOUBLE_EQ(state_.viewport.width(), width_after_first);

    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    ASSERT_TRUE(RunHandler());
    EXPECT_LT(state_.viewport.width(), width_after_first);
}

// ===================== Граничные случаи =====================

TEST(EdgeCases, ZeroMaxIterations) {
    EXPECT_EQ(CalculateIterationsForPoint({0.0, 0.0}, 0, 2.0), 0u);
    EXPECT_EQ(CalculateIterationsForPoint({10.0, 10.0}, 0, 2.0), 0u);
    EXPECT_TRUE(SameColor(IterationsToColor(0, 0), RgbColors::BLACK));
}

TEST(EdgeCases, ZeroEscapeRadius) {
    EXPECT_EQ(CalculateIterationsForPoint({0.0, 0.0}, 20, 0.0), 20u);
    EXPECT_EQ(CalculateIterationsForPoint({0.1, 0.0}, 20, 0.0), 1u);
}

TEST(EdgeCases, EscapeExactlyOnRadiusBoundaryIsNotEscape) {
    EXPECT_GT(CalculateIterationsForPoint({0.0, 2.0}, 100, 2.0), 1u);
}

TEST(EdgeCases, LastIterationBeforeMaxIsNotBlack) {
    const RgbColor c = IterationsToColor(99, 100);
    EXPECT_FALSE(SameColor(c, RgbColors::BLACK));
    EXPECT_EQ(c.r, 255);
}

TEST(EdgeCases, DegenerateViewportMapsAllPixelsToOnePoint) {
    const ViewPort vp{0.25, 0.25, -0.1, -0.1};
    EXPECT_DOUBLE_EQ(vp.width(), 0.0);
    const Complex a = Pixel2DToComplex(0, 0, vp, 10, 10);
    const Complex b = Pixel2DToComplex(9, 9, vp, 10, 10);
    EXPECT_DOUBLE_EQ(a.real(), b.real());
    EXPECT_DOUBLE_EQ(a.imag(), b.imag());
}

TEST(EdgeCases, FrameBufferSizes) {
    EXPECT_TRUE(FrameBuffer::Make(0, 0).rgba.empty());
    EXPECT_EQ(FrameBuffer::Make(1, 1).rgba.size(), 4u);
    EXPECT_EQ(FrameBuffer::Make(800, 600).rgba.size(), 800u * 600u * 4u);
}

TEST(EdgeCases, ComputeSenderOnSinglePixel) {
    RenderSettings settings{.width = 1, .height = 1, .max_iterations = 10, .escape_radius = 2.0};
    FrameBuffer fb = FrameBuffer::Make(1, 1);

    auto result = ex::sync_wait(ex::just(&fb) | MakeComputeSender(settings, AppState::INITIAL_VIEWPORT));
    ASSERT_TRUE(result.has_value());
    const RgbColor expected =
        IterationsToColor(CalculateIterationsForPoint({-2.5, -2.0}, settings.max_iterations, settings.escape_radius),
                          settings.max_iterations);
    EXPECT_TRUE(SameColor(PixelAt(fb, 0, 0), expected));
    EXPECT_EQ(fb.rgba[3], 255);
}

TEST(EdgeCases, ComputeSenderWithZeroIterationsPaintsBlack) {
    RenderSettings settings{.width = 8, .height = 6, .max_iterations = 0, .escape_radius = 2.0};
    FrameBuffer fb = FrameBuffer::Make(8, 6);
    ex::sync_wait(ex::just(&fb) | MakeComputeSender(settings, AppState::INITIAL_VIEWPORT));
    for (std::uint32_t y = 0; y < 6; ++y)
        for (std::uint32_t x = 0; x < 8; ++x)
            EXPECT_TRUE(SameColor(PixelAt(fb, x, y), RgbColors::BLACK));
}

TEST(EdgeCases, ComputeSenderOnDeepZoomViewport) {
    RenderSettings settings{.width = 16, .height = 12, .max_iterations = 50, .escape_radius = 2.0};
    const double cx = AppState::AUTO_ZOOM_TARGET_X, cy = AppState::AUTO_ZOOM_TARGET_Y, h = 1e-12;
    const ViewPort vp{cx - h, cx + h, cy - h, cy + h};
    FrameBuffer fb = FrameBuffer::Make(16, 12);
    auto result = ex::sync_wait(ex::just(&fb) | MakeComputeSender(settings, vp));
    ASSERT_TRUE(result.has_value());
    for (std::size_t i = 3; i < fb.rgba.size(); i += 4) {
        ASSERT_EQ(fb.rgba[i], 255);
    }
}

TEST_F(SfmlEventHandlerTest, AutoZoomDisabledDoesNothing) {
    state_.need_rerender = false;
    state_.auto_zoom_enabled = false;
    ASSERT_TRUE(RunHandler());
    EXPECT_FALSE(state_.need_rerender);
    EXPECT_DOUBLE_EQ(state_.viewport.x_min, AppState::INITIAL_VIEWPORT.x_min);
}

TEST_F(SfmlEventHandlerTest, RepeatedAutoZoomConvergesToTarget) {
    state_.auto_zoom_enabled = true;
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{110});
        ASSERT_TRUE(RunHandler());
        EXPECT_LT(state_.viewport.x_min, AppState::AUTO_ZOOM_TARGET_X);
        EXPECT_GT(state_.viewport.x_max, AppState::AUTO_ZOOM_TARGET_X);
    }
    EXPECT_LT(state_.viewport.width(), AppState::INITIAL_VIEWPORT.width() * 0.8 * 0.8 * 0.8 * 0.8);
}
