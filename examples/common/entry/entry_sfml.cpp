/*
 * Copyright 2011-2018 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause
 */

#include "entry_p.h"

#if ENTRY_CONFIG_USE_SFML

#if BX_PLATFORM_WINDOWS
#	define SDL_MAIN_HANDLED
#endif // BX_PLATFORM_WINDOWS

#include <bx/os.h>

#include <SFML/Window.hpp>

#include <bgfx/platform.h>

#include <bx/mutex.h>
#include <bx/thread.h>
#include <bx/handlealloc.h>
#include <bx/readerwriter.h>
#include <tinystl/allocator.h>
#include <tinystl/string.h>

namespace entry
{
	static uint8_t translateKeyModifiers(sf::Keyboard::Key key)
	{
		uint8_t modifiers = 0;
		modifiers |= key == sf::Keyboard::LAlt		? Modifier::LeftAlt    : 0;
		modifiers |= key == sf::Keyboard::RAlt		? Modifier::RightAlt   : 0;
		modifiers |= key == sf::Keyboard::LControl	? Modifier::LeftCtrl   : 0;
		modifiers |= key == sf::Keyboard::RControl	? Modifier::RightCtrl  : 0;
		modifiers |= key == sf::Keyboard::LShift	? Modifier::LeftShift  : 0;
		modifiers |= key == sf::Keyboard::RShift	? Modifier::RightShift : 0;
		modifiers |= key == sf::Keyboard::LSystem	? Modifier::LeftMeta   : 0;
		modifiers |= key == sf::Keyboard::RSystem	? Modifier::RightMeta  : 0;
		return modifiers;
	}

	static uint8_t translateKeyModifierPress(sf::Keyboard::Key _key)
	{
		uint8_t modifier;
		switch (_key)
		{
			case sf::Keyboard::LAlt		: { modifier = Modifier::LeftAlt;    } break;
			case sf::Keyboard::RAlt		: { modifier = Modifier::RightAlt;   } break;
			case sf::Keyboard::LControl	: { modifier = Modifier::LeftCtrl;   } break;
			case sf::Keyboard::RControl	: { modifier = Modifier::RightCtrl;  } break;
			case sf::Keyboard::LShift	: { modifier = Modifier::LeftShift;  } break;
			case sf::Keyboard::RShift	: { modifier = Modifier::RightShift; } break;
			case sf::Keyboard::LSystem	: { modifier = Modifier::LeftMeta;   } break;
			case sf::Keyboard::RSystem	: { modifier = Modifier::RightMeta;  } break;
			default:                  { modifier = 0;                    } break;
		}

		return modifier;
	}

	static uint8_t s_translateKey[256];

	static void initTranslateKey(sf::Keyboard::Key _sfml, Key::Enum _key)
	{
		BX_CHECK(_sdl < BX_COUNTOF(s_translateKey), "Out of bounds %d.", _sdl);
		s_translateKey[_sfml] = (uint8_t)_key;
	}

	static Key::Enum translateKey(sf::Keyboard::Key _sfml)
	{
		return (Key::Enum)s_translateKey[_sfml];
	}

	static uint8_t s_translateGamepad[256];

	static void initTranslateGamepad(uint8_t _sfml, Key::Enum _button)
	{
		s_translateGamepad[_sfml] = _button;
	}

	static Key::Enum translateGamepad(uint8_t _sfml)
	{
		return Key::Enum(s_translateGamepad[_sfml]);
	}

	static uint8_t s_translateGamepadAxis[256];

	static void initTranslateGamepadAxis(sf::Joystick::Axis _sfml, GamepadAxis::Enum _axis)
	{
		s_translateGamepadAxis[_sfml] = uint8_t(_axis);
	}

	static GamepadAxis::Enum translateGamepadAxis(uint8_t _sfml)
	{
		return GamepadAxis::Enum(s_translateGamepadAxis[_sfml]);
	}

	struct AxisDpadRemap
	{
		Key::Enum first;
		Key::Enum second;
	};

	static AxisDpadRemap s_axisDpad[] =
	{
		{ Key::GamepadLeft, Key::GamepadRight },
		{ Key::GamepadUp,   Key::GamepadDown  },
		{ Key::None,        Key::None         },
		{ Key::GamepadLeft, Key::GamepadRight },
		{ Key::GamepadUp,   Key::GamepadDown  },
		{ Key::None,        Key::None         },
	};

	struct GamepadSFML
	{
		GamepadSFML()
			: m_jid(INT32_MAX)
		{
			bx::memSet(m_value, 0, sizeof(m_value) );

			// Deadzone values from xinput.h
			m_deadzone[GamepadAxis::LeftX ] =
			m_deadzone[GamepadAxis::LeftY ] = 7849;
			m_deadzone[GamepadAxis::RightX] =
			m_deadzone[GamepadAxis::RightY] = 8689;
			m_deadzone[GamepadAxis::LeftZ ] =
			m_deadzone[GamepadAxis::RightZ] = 30;
		}

		void create(const sf::Event::JoystickConnectEvent& _jev)
		{
			m_jid = _jev.joystickId;
		}

		void update(EventQueue& _eventQueue, WindowHandle _handle, GamepadHandle _gamepad, GamepadAxis::Enum _axis, int32_t _value)
		{
			if (filter(_axis, &_value) )
			{
				_eventQueue.postAxisEvent(_handle, _gamepad, _axis, _value);

				if (Key::None != s_axisDpad[_axis].first)
				{
					if (_value == 0)
					{
						_eventQueue.postKeyEvent(_handle, s_axisDpad[_axis].first,  0, false);
						_eventQueue.postKeyEvent(_handle, s_axisDpad[_axis].second, 0, false);
					}
					else
					{
						_eventQueue.postKeyEvent(_handle
								, 0 > _value ? s_axisDpad[_axis].first : s_axisDpad[_axis].second
								, 0
								, true
								);
					}
				}
			}
		}

		void destroy()
		{
			m_jid = INT32_MAX;
		}

		bool filter(GamepadAxis::Enum _axis, int32_t* _value)
		{
			const int32_t old = m_value[_axis];
			const int32_t deadzone = m_deadzone[_axis];
			int32_t value = *_value;
			value = value > deadzone || value < -deadzone ? value : 0;
			m_value[_axis] = value;
			*_value = value;
			return old != value;
		}

		int32_t m_value[GamepadAxis::Count];
		int32_t m_deadzone[GamepadAxis::Count];

		unsigned int      m_jid;
	};

	struct MainThreadEntry
	{
		int m_argc;
		char** m_argv;

		static int32_t threadFunc(bx::Thread* _thread, void* _userData);
	};


	struct Msg
	{
		Msg()
			: m_x(0)
			, m_y(0)
			, m_width(0)
			, m_height(0)
			, m_flags(0)
			, m_flagsEnabled(false)
		{
		}

		int32_t  m_x;
		int32_t  m_y;
		uint32_t m_width;
		uint32_t m_height;
		uint32_t m_flags;
		tinystl::string m_title;
		bool m_flagsEnabled;
	};

	enum SDL_USER_WINDOW
	{
		SDL_USER_WINDOW_CREATE,
		SDL_USER_WINDOW_DESTROY,
		SDL_USER_WINDOW_SET_TITLE,
		SDL_USER_WINDOW_SET_FLAGS,
		SDL_USER_WINDOW_SET_POS,
		SDL_USER_WINDOW_SET_SIZE,
		SDL_USER_WINDOW_TOGGLE_FRAME,
		SDL_USER_WINDOW_TOGGLE_FULL_SCREEN,
		SDL_USER_WINDOW_MOUSE_LOCK,
	};

	struct Context
	{
		Context()
			: m_width(ENTRY_DEFAULT_WIDTH)
			, m_height(ENTRY_DEFAULT_HEIGHT)
			, m_aspectRatio(16.0f/9.0f)
			, m_mx(0)
			, m_my(0)
			, m_mz(0)
			, m_mouseLock(false)
			, m_fullscreen(false)
		{
			bx::memSet(s_translateKey, 0, sizeof(s_translateKey) );
			initTranslateKey(sf::Keyboard::Escape,       Key::Esc);
			initTranslateKey(sf::Keyboard::Return,       Key::Return);
			initTranslateKey(sf::Keyboard::Tab,          Key::Tab);
			initTranslateKey(sf::Keyboard::Backspace,    Key::Backspace);
			initTranslateKey(sf::Keyboard::Space,        Key::Space);
			initTranslateKey(sf::Keyboard::Up,           Key::Up);
			initTranslateKey(sf::Keyboard::Down,         Key::Down);
			initTranslateKey(sf::Keyboard::Left,         Key::Left);
			initTranslateKey(sf::Keyboard::Right,        Key::Right);
			initTranslateKey(sf::Keyboard::PageUp,       Key::PageUp);
			initTranslateKey(sf::Keyboard::PageDown,     Key::PageDown);
			initTranslateKey(sf::Keyboard::Home,         Key::Home);
			initTranslateKey(sf::Keyboard::End,          Key::End);
			initTranslateKey(sf::Keyboard::Add,      Key::Plus);
			initTranslateKey(sf::Keyboard::Subtract,     Key::Minus);
			initTranslateKey(sf::Keyboard::Tilde,        Key::Tilde);
			initTranslateKey(sf::Keyboard::Comma,     Key::Comma);
			initTranslateKey(sf::Keyboard::Period,    Key::Period);
			initTranslateKey(sf::Keyboard::Slash,        Key::Slash);
			initTranslateKey(sf::Keyboard::F1,           Key::F1);
			initTranslateKey(sf::Keyboard::F2,           Key::F2);
			initTranslateKey(sf::Keyboard::F3,           Key::F3);
			initTranslateKey(sf::Keyboard::F4,           Key::F4);
			initTranslateKey(sf::Keyboard::F5,           Key::F5);
			initTranslateKey(sf::Keyboard::F6,           Key::F6);
			initTranslateKey(sf::Keyboard::F7,           Key::F7);
			initTranslateKey(sf::Keyboard::F8,           Key::F8);
			initTranslateKey(sf::Keyboard::F9,           Key::F9);
			initTranslateKey(sf::Keyboard::F10,          Key::F10);
			initTranslateKey(sf::Keyboard::F11,          Key::F11);
			initTranslateKey(sf::Keyboard::F12,          Key::F12);
			initTranslateKey(sf::Keyboard::Numpad0,         Key::NumPad0);
			initTranslateKey(sf::Keyboard::Numpad1,         Key::NumPad1);
			initTranslateKey(sf::Keyboard::Numpad2,         Key::NumPad2);
			initTranslateKey(sf::Keyboard::Numpad3,         Key::NumPad3);
			initTranslateKey(sf::Keyboard::Numpad4,         Key::NumPad4);
			initTranslateKey(sf::Keyboard::Numpad5,         Key::NumPad5);
			initTranslateKey(sf::Keyboard::Numpad6,         Key::NumPad6);
			initTranslateKey(sf::Keyboard::Numpad7,         Key::NumPad7);
			initTranslateKey(sf::Keyboard::Numpad8,         Key::NumPad8);
			initTranslateKey(sf::Keyboard::Numpad9,         Key::NumPad9);
			initTranslateKey(sf::Keyboard::Num0,            Key::Key0);
			initTranslateKey(sf::Keyboard::Num1,            Key::Key1);
			initTranslateKey(sf::Keyboard::Num2,            Key::Key2);
			initTranslateKey(sf::Keyboard::Num3,            Key::Key3);
			initTranslateKey(sf::Keyboard::Num4,            Key::Key4);
			initTranslateKey(sf::Keyboard::Num5,            Key::Key5);
			initTranslateKey(sf::Keyboard::Num6,            Key::Key6);
			initTranslateKey(sf::Keyboard::Num7,            Key::Key7);
			initTranslateKey(sf::Keyboard::Num8,            Key::Key8);
			initTranslateKey(sf::Keyboard::Num9,            Key::Key9);
			initTranslateKey(sf::Keyboard::A,            Key::KeyA);
			initTranslateKey(sf::Keyboard::B,            Key::KeyB);
			initTranslateKey(sf::Keyboard::C,            Key::KeyC);
			initTranslateKey(sf::Keyboard::D,            Key::KeyD);
			initTranslateKey(sf::Keyboard::E,            Key::KeyE);
			initTranslateKey(sf::Keyboard::F,            Key::KeyF);
			initTranslateKey(sf::Keyboard::G,            Key::KeyG);
			initTranslateKey(sf::Keyboard::H,            Key::KeyH);
			initTranslateKey(sf::Keyboard::I,            Key::KeyI);
			initTranslateKey(sf::Keyboard::J,            Key::KeyJ);
			initTranslateKey(sf::Keyboard::K,            Key::KeyK);
			initTranslateKey(sf::Keyboard::L,            Key::KeyL);
			initTranslateKey(sf::Keyboard::M,            Key::KeyM);
			initTranslateKey(sf::Keyboard::N,            Key::KeyN);
			initTranslateKey(sf::Keyboard::O,            Key::KeyO);
			initTranslateKey(sf::Keyboard::P,            Key::KeyP);
			initTranslateKey(sf::Keyboard::Q,            Key::KeyQ);
			initTranslateKey(sf::Keyboard::R,            Key::KeyR);
			initTranslateKey(sf::Keyboard::S,            Key::KeyS);
			initTranslateKey(sf::Keyboard::T,            Key::KeyT);
			initTranslateKey(sf::Keyboard::U,            Key::KeyU);
			initTranslateKey(sf::Keyboard::V,            Key::KeyV);
			initTranslateKey(sf::Keyboard::W,            Key::KeyW);
			initTranslateKey(sf::Keyboard::X,            Key::KeyX);
			initTranslateKey(sf::Keyboard::Y,            Key::KeyY);
			initTranslateKey(sf::Keyboard::Z,            Key::KeyZ);

			// Complete stab in the dark...
			bx::memSet(s_translateGamepad, uint8_t(Key::Count), sizeof(s_translateGamepad) );
			initTranslateGamepad(0,             Key::GamepadA);
			initTranslateGamepad(1,             Key::GamepadB);
			initTranslateGamepad(2,             Key::GamepadX);
			initTranslateGamepad(3,             Key::GamepadY);
			initTranslateGamepad(4,     Key::GamepadThumbL);
			initTranslateGamepad(5,    Key::GamepadThumbR);
			initTranslateGamepad(6,  Key::GamepadShoulderL);
			initTranslateGamepad(7, Key::GamepadShoulderR);
			initTranslateGamepad(8,       Key::GamepadUp);
			initTranslateGamepad(9,     Key::GamepadDown);
			initTranslateGamepad(10,     Key::GamepadLeft);
			initTranslateGamepad(11,    Key::GamepadRight);
			initTranslateGamepad(12,          Key::GamepadBack);
			initTranslateGamepad(13,         Key::GamepadStart);
			initTranslateGamepad(14,         Key::GamepadGuide);

			bx::memSet(s_translateGamepadAxis, uint8_t(GamepadAxis::Count), sizeof(s_translateGamepadAxis) );
			initTranslateGamepadAxis(sf::Joystick::Axis::X,        GamepadAxis::LeftX);
			initTranslateGamepadAxis(sf::Joystick::Axis::Y,        GamepadAxis::LeftY);
			initTranslateGamepadAxis(sf::Joystick::Axis::R,  GamepadAxis::LeftZ);
			initTranslateGamepadAxis(sf::Joystick::Axis::Z,       GamepadAxis::RightX);
			initTranslateGamepadAxis(sf::Joystick::Axis::V,       GamepadAxis::RightY);
			initTranslateGamepadAxis(sf::Joystick::Axis::U, GamepadAxis::RightZ);
		}

		int run(int _argc, char** _argv)
		{
			m_mte.m_argc = _argc;
			m_mte.m_argv = _argv;

			m_windowAlloc.alloc();
			m_window.create({ m_width, m_height },"bgfx");

			m_flags[0] = 0
				| ENTRY_WINDOW_FLAG_ASPECT_RATIO
				| ENTRY_WINDOW_FLAG_FRAME
				;

			bgfx::PlatformData pd;
			pd.ndt = NULL;
			pd.nwh = m_window.getSystemHandle();
			bgfx::setPlatformData(pd);
			bgfx::renderFrame();

			m_thread.init(MainThreadEntry::threadFunc, &m_mte);

			// Force window resolution...
			WindowHandle defaultWindow = { 0 };
			setWindowSize(defaultWindow, m_width, m_height, true);

			bool exit = false;
			sf::Event event;
			while (!exit)
			{
				bgfx::renderFrame();

				while (m_window.pollEvent(event) )
				{
					switch (event.type)
					{
					case sf::Event::Closed:
						m_eventQueue.postExitEvent();
						exit = true;
						break;

					case sf::Event::MouseMoved:
						{
							m_mx = event.mouseMove.x;
							m_my = event.mouseMove.y;

							m_eventQueue.postMouseEvent(defaultWindow, m_mx, m_my, m_mz);
						}
						break;

					case sf::Event::MouseButtonPressed:
					case sf::Event::MouseButtonReleased:
						{
							MouseButton::Enum button;
							switch (event.mouseButton.button)
							{
							default:
							case sf::Mouse::Left:   button = MouseButton::Left;   break;
							case sf::Mouse::Middle: button = MouseButton::Middle; break;
							case sf::Mouse::Right:  button = MouseButton::Right;  break;
							}

							m_eventQueue.postMouseEvent(defaultWindow
								, event.mouseButton.x
								, event.mouseButton.y
								, m_mz
								, button
								, event.type == sf::Event::MouseButtonPressed
								);
						}
						break;

					case sf::Event::MouseWheelScrolled:
						{
							m_mz += event.mouseWheelScroll.delta;
							m_eventQueue.postMouseEvent(defaultWindow, m_mx, m_my, m_mz);
						}
						break;

					case sf::Event::TextEntered:
						{
							m_eventQueue.postCharEvent(defaultWindow, 1, (const uint8_t*)event.text.unicode);
						}
						break;

					case sf::Event::KeyPressed:
						{
							Key::Enum key = translateKey(event.key.code);

							if (Key::Esc == key)
							{
								uint8_t pressedChar[4];
								pressedChar[0] = 0x1b;
								m_eventQueue.postCharEvent(defaultWindow, 1, pressedChar);
							}
							else if (Key::Return == key)
							{
								uint8_t pressedChar[4];
								pressedChar[0] = 0x0d;
								m_eventQueue.postCharEvent(defaultWindow, 1, pressedChar);
							}
							else if (Key::Backspace == key)
							{
								uint8_t pressedChar[4];
								pressedChar[0] = 0x08;
								m_eventQueue.postCharEvent(defaultWindow, 1, pressedChar);
							}

							m_eventQueue.postKeyEvent(defaultWindow, key, 0, true);
						}
						break;

					case sf::Event::KeyReleased:
						{
						Key::Enum key = translateKey(event.key.code);
						m_eventQueue.postKeyEvent(defaultWindow, key, 0, false);
						}
						break;

					case sf::Event::Resized:
						{
							setWindowSize(defaultWindow, event.size.width, event.size.height);
						}
						break;
					}
				}
			}

			while (bgfx::RenderFrame::NoContext != bgfx::renderFrame() ) {};
			m_thread.shutdown();

			m_window.close();

			return m_thread.getExitCode();
		}

		void setWindowSize(WindowHandle _handle, uint32_t _width, uint32_t _height, bool _force = false)
		{
			if (_width  != m_width
			||  _height != m_height
			||  _force)
			{
				m_width  = _width;
				m_height = _height;

				m_window.setSize({ m_width, m_height });
				m_eventQueue.postSizeEvent({ 0 }, m_width, m_height);
			}
		}

		MainThreadEntry m_mte;
		bx::Thread m_thread;

		EventQueue m_eventQueue;
		bx::Mutex m_lock;

		bx::HandleAllocT<ENTRY_CONFIG_MAX_WINDOWS> m_windowAlloc;
		sf::Window m_window;
		uint32_t m_flags[ENTRY_CONFIG_MAX_WINDOWS];

		bx::HandleAllocT<ENTRY_CONFIG_MAX_GAMEPADS> m_gamepadAlloc;

		uint32_t m_width;
		uint32_t m_height;
		float m_aspectRatio;

		int32_t m_mx;
		int32_t m_my;
		int32_t m_mz;
		bool m_mouseLock;
		bool m_fullscreen;
	};

	static Context s_ctx;

	const Event* poll()
	{
		return s_ctx.m_eventQueue.poll();
	}

	const Event* poll(WindowHandle _handle)
	{
		return s_ctx.m_eventQueue.poll(_handle);
	}

	void release(const Event* _event)
	{
		s_ctx.m_eventQueue.release(_event);
	}

	WindowHandle createWindow(int32_t _x, int32_t _y, uint32_t _width, uint32_t _height, uint32_t _flags, const char* _title)
	{
		bx::MutexScope scope(s_ctx.m_lock);
		WindowHandle handle = { s_ctx.m_windowAlloc.alloc() };

		if (UINT16_MAX != handle.idx)
		{
			Msg* msg = new Msg;
			msg->m_x      = _x;
			msg->m_y      = _y;
			msg->m_width  = _width;
			msg->m_height = _height;
			msg->m_title  = _title;
			msg->m_flags  = _flags;

			s_ctx.m_window.create({ _width, _height }, _title);
		}

		return handle;
	}

	void destroyWindow(WindowHandle _handle)
	{
		if (UINT16_MAX != _handle.idx)
		{
			s_ctx.m_window.close();

			bx::MutexScope scope(s_ctx.m_lock);
			s_ctx.m_windowAlloc.free(_handle.idx);
		}
	}

	void setWindowPos(WindowHandle _handle, int32_t _x, int32_t _y)
	{
		Msg* msg = new Msg;
		msg->m_x = _x;
		msg->m_y = _y;

		s_ctx.m_window.setPosition({ _x,_y });
	}

	void setWindowSize(WindowHandle _handle, uint32_t _width, uint32_t _height)
	{
		Msg* msg = new Msg;
		msg->m_width  = _width;
		msg->m_height = _height;

		s_ctx.m_window.setSize({ _width,_height });
	}

	void setWindowTitle(WindowHandle _handle, const char* _title)
	{
		Msg* msg = new Msg;
		msg->m_title = _title;

		s_ctx.m_window.setTitle(_title);
	}

	void setWindowFlags(WindowHandle _handle, uint32_t _flags, bool _enabled)
	{
		Msg* msg = new Msg;
		msg->m_flags = _flags;
		msg->m_flagsEnabled = _enabled;
		//umm....
	}

	void toggleFullscreen(WindowHandle _handle)
	{
		//umm....
	}

	void setMouseLock(WindowHandle _handle, bool _lock)
	{
		//s_ctx.m_window.setMouseCursorGrabbed(_lock);
	}

	int32_t MainThreadEntry::threadFunc(bx::Thread* _thread, void* _userData)
	{
		BX_UNUSED(_thread);

		MainThreadEntry* self = (MainThreadEntry*)_userData;
		int32_t result = main(self->m_argc, self->m_argv);

		return result;
	}

} // namespace entry

int main(int _argc, char** _argv)
{
	using namespace entry;
	return s_ctx.run(_argc, _argv);
}

#endif // ENTRY_CONFIG_USE_SDL
