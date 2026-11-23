#pragma once

#include <SFML/Graphics.hpp>
#include <stdexec/execution.hpp>

#include <cmath>
#include <exception>
#include <utility>

#include "types_core.hpp"

class SfmlEventHandler {
public:
    using sender_concept = stdexec::sender_tag;
    using completion_signatures_t =
        stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_error_t(std::exception_ptr),
                                       stdexec::set_stopped_t()>;

    template <typename Receiver>
    struct OperationState {
        using operation_state_concept = stdexec::operation_state_tag;

        Receiver receiver_;
        sf::RenderWindow &window_;
        RenderSettings &render_settings_;
        AppState &state_;

        static constexpr float ZOOM_INTERVAL_MS = 100.0f;

        template <typename R>
        explicit OperationState(R &&r, sf::RenderWindow &window, RenderSettings &render_settings, AppState &state)
            : receiver_{std::forward<R>(r)}, window_{window}, render_settings_{render_settings}, state_{state} {}

        OperationState(OperationState &&) = delete;
        OperationState &operator=(OperationState &&) = delete;

        void start() & noexcept {
            try {
                HandleEvents();
                if (state_.should_exit) {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
                HandleHeldMouse();
                HandleAutoZoom();
            } catch (...) {
                stdexec::set_error(std::move(receiver_), std::current_exception());
                return;
            }
            stdexec::set_value(std::move(receiver_));
        }

    private:
        void HandleEvents() {
            sf::Event event;
            while (window_.pollEvent(event)) {
                switch (event.type) {
                case sf::Event::Closed:
                    state_.should_exit = true;
                    break;

                case sf::Event::KeyPressed:
                    HandleKeyPress(event.key);
                    break;

                case sf::Event::MouseButtonPressed:
                    HandleMousePress(event.mouseButton);
                    break;

                case sf::Event::MouseButtonReleased:
                    HandleMouseRelease(event.mouseButton);
                    break;

                default:
                    break;
                }
            }
        }

        void HandleKeyPress(const sf::Event::KeyEvent &key) {
            static constexpr double delta_radius = 0.5;
            static constexpr double delta_iterations = 1;

            if (key.code == sf::Keyboard::X) {
                state_.auto_zoom_enabled = !state_.auto_zoom_enabled;
            } else if (key.code == sf::Keyboard::C) {
                state_.auto_zoom_enabled = false;
                state_.viewport = AppState::INITIAL_VIEWPORT;
                state_.need_rerender = true;
                state_.zoom_clock.restart();
            } else if (key.code == sf::Keyboard::Q) {
                state_.auto_zoom_enabled = false;
                state_.viewport = AppState::INITIAL_VIEWPORT;
                state_.need_rerender = true;
                render_settings_.escape_radius -= delta_radius;
                state_.zoom_clock.restart();

            } else if (key.code == sf::Keyboard::W) {
                state_.auto_zoom_enabled = false;
                state_.viewport = AppState::INITIAL_VIEWPORT;
                state_.need_rerender = true;
                render_settings_.escape_radius += delta_radius;
                state_.zoom_clock.restart();
            } else if (key.code == sf::Keyboard::E) {
                state_.auto_zoom_enabled = false;
                state_.viewport = AppState::INITIAL_VIEWPORT;
                state_.need_rerender = true;
                render_settings_.max_iterations -= delta_iterations;
                state_.zoom_clock.restart();

            } else if (key.code == sf::Keyboard::R) {
                state_.auto_zoom_enabled = false;
                state_.viewport = AppState::INITIAL_VIEWPORT;
                state_.need_rerender = true;
                render_settings_.max_iterations += delta_iterations;
                state_.zoom_clock.restart();
            }
        }

        void HandleMousePress(const sf::Event::MouseButtonEvent &mouse) {
            if (mouse.button == sf::Mouse::Left) {
                state_.left_mouse_pressed = true;
                ZoomToPoint(mouse.x, mouse.y, true);
            } else if (mouse.button == sf::Mouse::Right) {
                state_.right_mouse_pressed = true;
                ZoomToPoint(mouse.x, mouse.y, false);
            }
        }

        void HandleMouseRelease(const sf::Event::MouseButtonEvent &mouse) {
            if (mouse.button == sf::Mouse::Left) {
                state_.left_mouse_pressed = false;
            } else if (mouse.button == sf::Mouse::Right) {
                state_.right_mouse_pressed = false;
            }
        }

        // Пока кнопка удерживается, зум продолжается с интервалом ZOOM_INTERVAL_MS.
        void HandleHeldMouse() {
            if (!state_.left_mouse_pressed && !state_.right_mouse_pressed) {
                return;
            }
            const sf::Vector2i pos = sf::Mouse::getPosition(window_);
            ZoomToPoint(pos.x, pos.y, state_.left_mouse_pressed);
        }

        void HandleAutoZoom() {
            if (!state_.auto_zoom_enabled) {
                return;
            }
            const double px = (AppState::AUTO_ZOOM_TARGET_X - state_.viewport.x_min) / state_.viewport.width() *
                              render_settings_.width;
            const double py = (AppState::AUTO_ZOOM_TARGET_Y - state_.viewport.y_min) / state_.viewport.height() *
                              render_settings_.height;
            ZoomToPoint(static_cast<int>(std::lround(px)), static_cast<int>(std::lround(py)), true);
        }

        bool ZoomIntervalElapsed() const {
            return state_.zoom_clock.getElapsedTime().asMilliseconds() >= ZOOM_INTERVAL_MS;
        }

        void ZoomToPoint(int pixel_x, int pixel_y, bool zoom_in, double factor = 0.8) {
            if (!ZoomIntervalElapsed()) {
                return;
            }

            const double target_x = state_.viewport.x_min +
                                    (static_cast<double>(pixel_x) / render_settings_.width) * state_.viewport.width();
            const double target_y = state_.viewport.y_min +
                                    (static_cast<double>(pixel_y) / render_settings_.height) * state_.viewport.height();

            const double zoom_factor = zoom_in ? factor : (1.0 / factor);
            const double new_width = state_.viewport.width() * zoom_factor;
            const double new_height = state_.viewport.height() * zoom_factor;

            state_.viewport.x_min = target_x - new_width / 2.0;
            state_.viewport.x_max = target_x + new_width / 2.0;
            state_.viewport.y_min = target_y - new_height / 2.0;
            state_.viewport.y_max = target_y + new_height / 2.0;

            state_.need_rerender = true;
            state_.zoom_clock.restart();
        }
    };

    sf::RenderWindow &window_;
    RenderSettings &render_settings_;
    AppState &state_;

    SfmlEventHandler(sf::RenderWindow &window, RenderSettings &render_settings, AppState &state)
        : window_{window}, render_settings_{render_settings}, state_{state} {}

    template <stdexec::receiver_of<completion_signatures_t> Receiver>
    auto connect(Receiver receiver) const -> OperationState<Receiver> {
        return OperationState<Receiver>{std::move(receiver), window_, render_settings_, state_};
    }

    template <typename Self, typename... Env>
    static consteval auto get_completion_signatures() noexcept {
        return completion_signatures_t{};
    }
};
