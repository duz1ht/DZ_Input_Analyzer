// plugins/dz-input-analyzer/dz-input-analyzer.cpp
#include <obs-module.h>

#undef LIBOBS_API_VER
#define LIBOBS_API_VER obs_get_version()
#include <graphics/graphics.h>
#include <graphics/vec4.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hidusage.h>

#include <atomic>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <deque>
#include <chrono>
#include <cmath>
#include <string>
#include <cstring>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("dz-input-analyzer", "pt-BR")

static const wchar_t *kWndClass = L"DZ_Analisador_Entrada_Janela";

// ------------------------------------------------------------
// Utilitários de tempo
static inline int64_t now_ms()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------
// Modelo da linha do tempo (equivalente à lógica do movv.html)
enum dz_row : int { ROW_W = 0, ROW_S = 1, ROW_A = 2, ROW_D = 3, ROW_COUNT = 4 };

struct dz_key_segment {
	int row = 0;          // 0..3
	int64_t start_ms = 0; // ms absoluto
	int64_t end_ms = -1;  // -1 enquanto pressionado
};

struct dz_click_event {
	int row = ROW_D;     // linha para exibir o delta
	int64_t time_ms = 0; // ms absoluto
	int delta_ms = 0;    // >= 0
};

struct dz_input_state {
	std::atomic<uint8_t> key_down[ROW_COUNT]{{0}, {0}, {0}, {0}};
	std::atomic<uint8_t> m1{0}, m2{0}, m3{0};

	std::atomic<int> last_dx{0};
	std::atomic<int> last_dy{0};

	std::atomic<int64_t> total_dx{0};
	std::atomic<int64_t> total_dy{0};

	std::atomic<uint32_t> mouse_events{0};
	std::atomic<uint32_t> key_events{0};
};

struct dz_source_data {
	// Tamanho da fonte
	uint32_t width = 1500;
	uint32_t height = 520;

	// Configuração visual
	float bg_alpha = 0.55f;
	bool row_enabled[ROW_COUNT] = {true, true, true, true};
	uint16_t row_key_vkey[ROW_COUNT] = {'W', 'S', 'A', 'D'};

	// Cores (o seletor do OBS retorna BGR: 0x00BBGGRR)
	uint32_t bg_color = 0x000000; // RGB do fundo em BGR do OBS (padrão preto)
	uint32_t key_color[ROW_COUNT] = {
		0x005dc8f3, // W: #f3c85d
		0x009cff9c, // S: #9cff9c
		0x003f3fcf, // A: #cf3f3f
		0x00c8a00a  // D: #0aa0c8
	};

	// Efeito do OBS
	gs_effect_t *solid = nullptr;

	// Janela de entrada bruta
	HWND hwnd = nullptr;

	// Estado de entrada (bruta)
	dz_input_state st;

	// Armazenamento da linha do tempo
	std::deque<dz_key_segment> segments;
	std::deque<dz_click_event> clicks;

	// Comportamento do movv.html: guarda a "última tecla pressionada" (linha + tempo)
	std::atomic<int> last_key_row{ROW_D};
	std::atomic<int64_t> last_key_down_ms{0};
	std::atomic<int> last_key_valid{0};

	// Controle interno
	uint64_t frame_counter = 0;
};

static inline int vkey_to_row(const dz_source_data *d, uint16_t vkey)
{
	if (!d)
		return -1;
	for (int i = 0; i < ROW_COUNT; i++) {
		if (d->row_key_vkey[i] == vkey)
			return i;
	}
	return -1;
}

struct dz_key_option {
	uint16_t vkey;
	const char *nome;
};

static const dz_key_option kKeyOptions[] = {
	{'A', "A"},
	{'B', "B"},
	{'C', "C"},
	{'D', "D"},
	{'E', "E"},
	{'F', "F"},
	{'G', "G"},
	{'H', "H"},
	{'I', "I"},
	{'J', "J"},
	{'K', "K"},
	{'L', "L"},
	{'M', "M"},
	{'N', "N"},
	{'O', "O"},
	{'P', "P"},
	{'Q', "Q"},
	{'R', "R"},
	{'S', "S"},
	{'T', "T"},
	{'U', "U"},
	{'V', "V"},
	{'W', "W"},
	{'X', "X"},
	{'Y', "Y"},
	{'Z', "Z"},
	{'0', "0"},
	{'1', "1"},
	{'2', "2"},
	{'3', "3"},
	{'4', "4"},
	{'5', "5"},
	{'6', "6"},
	{'7', "7"},
	{'8', "8"},
	{'9', "9"},
	{VK_LEFT, "SETA ESQUERDA"},
	{VK_RIGHT, "SETA DIREITA"},
	{VK_UP, "SETA CIMA"},
	{VK_DOWN, "SETA BAIXO"},
	{VK_SPACE, "ESPACO"},
	{VK_RETURN, "ENTRAR"},
	{VK_TAB, "TAB"},
	{VK_ESCAPE, "ESC"},
	{VK_SHIFT, "MAIUSC"},
	{VK_CONTROL, "CTRL"},
	{VK_MENU, "ALT"},
	{VK_F1, "F1"},
	{VK_F2, "F2"},
	{VK_F3, "F3"},
	{VK_F4, "F4"},
	{VK_F5, "F5"},
	{VK_F6, "F6"},
	{VK_F7, "F7"},
	{VK_F8, "F8"},
	{VK_F9, "F9"},
	{VK_F10, "F10"},
	{VK_F11, "F11"},
	{VK_F12, "F12"},
};

static const char *dz_nome_tecla(uint16_t vkey)
{
	for (const auto &opt : kKeyOptions) {
		if (opt.vkey == vkey)
			return opt.nome;
	}
	return "DESCONHECIDA";
}

static std::string dz_titulo_tecla(uint16_t vkey)
{
	std::string titulo = "Tecla ";
	titulo += dz_nome_tecla(vkey);
	return titulo;
}

// ------------------------------------------------------------
// Janela oculta + entrada bruta
static LRESULT CALLBACK dz_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	if (msg == WM_NCCREATE) {
		auto *cs = (CREATESTRUCTW *)lparam;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}

	auto *d = (dz_source_data *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

	switch (msg) {
	case WM_INPUT: {
		if (!d)
			break;

		UINT size = 0;
		if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0)
			break;
		if (size == 0 || size > (1u << 16))
			break;

		uint8_t buf[1u << 13]; // 8192
		if (size > sizeof(buf))
			break;

		if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size)
			break;

		const RAWINPUT *ri = (const RAWINPUT *)buf;

		if (ri->header.dwType == RIM_TYPEMOUSE) {
			const RAWMOUSE &m = ri->data.mouse;

			int dx = (int)m.lLastX;
			int dy = (int)m.lLastY;

			d->st.last_dx.store(dx, std::memory_order_relaxed);
			d->st.last_dy.store(dy, std::memory_order_relaxed);

			d->st.total_dx.fetch_add(dx, std::memory_order_relaxed);
			d->st.total_dy.fetch_add(dy, std::memory_order_relaxed);

			d->st.mouse_events.fetch_add(1, std::memory_order_relaxed);

			USHORT bf = m.usButtonFlags;

			// Botões
			if (bf & RI_MOUSE_BUTTON_1_DOWN) {
				d->st.m1.store(1, std::memory_order_relaxed);

				// movv.html: clique esquerdo cria marcador e delta da última tecla pressionada
				const int64_t t = now_ms();

				int row = ROW_D;
				int delta = 0;

				if (d->last_key_valid.load(std::memory_order_relaxed) != 0) {
					row = d->last_key_row.load(std::memory_order_relaxed);
					const int64_t lk = d->last_key_down_ms.load(std::memory_order_relaxed);
					const int64_t raw_delta = t - lk;
					delta = (raw_delta > 0) ? (int)raw_delta : 0;
				}

				d->clicks.push_back({row, t, delta});
			}
			if (bf & RI_MOUSE_BUTTON_1_UP)
				d->st.m1.store(0, std::memory_order_relaxed);
			if (bf & RI_MOUSE_BUTTON_2_DOWN)
				d->st.m2.store(1, std::memory_order_relaxed);
			if (bf & RI_MOUSE_BUTTON_2_UP)
				d->st.m2.store(0, std::memory_order_relaxed);
			if (bf & RI_MOUSE_BUTTON_3_DOWN)
				d->st.m3.store(1, std::memory_order_relaxed);
			if (bf & RI_MOUSE_BUTTON_3_UP)
				d->st.m3.store(0, std::memory_order_relaxed);

		} else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
			const RAWKEYBOARD &k = ri->data.keyboard;

			const bool is_break = (k.Flags & RI_KEY_BREAK) != 0;
			const uint16_t vkey = (uint16_t)k.VKey;

			const int row = vkey_to_row(d, vkey);
			bool was_down = false;
			if (row != -1) {
				was_down = d->st.key_down[row].load(std::memory_order_relaxed) != 0;
				d->st.key_down[row].store(is_break ? 0 : 1, std::memory_order_relaxed);
			}

			// Segmentos da linha do tempo: apenas as 4 teclas configuradas
			if (row != -1) {
				const int64_t t = now_ms();

				if (!is_break) {
					// pressionar tecla (ignorar repetição quando já está pressionada)
					bool already = was_down;

					// Inicia um segmento apenas se não houver um aberto para essa linha.
					bool has_open = false;
					for (auto it = d->segments.rbegin(); it != d->segments.rend(); ++it) {
						if (it->row == row && it->end_ms < 0) {
							has_open = true;
							break;
						}
					}
					if (!already && !has_open) {
						d->segments.push_back({row, t, -1});
						d->last_key_row.store(row, std::memory_order_relaxed);
						d->last_key_down_ms.store(t, std::memory_order_relaxed);
						d->last_key_valid.store(1, std::memory_order_relaxed);
					}
				} else {
					// soltar tecla: fecha o último segmento aberto dessa linha
					for (auto it = d->segments.rbegin(); it != d->segments.rend(); ++it) {
						if (it->row == row && it->end_ms < 0) {
							it->end_ms = t;
							break;
						}
					}
				}
			}

			d->st.key_events.fetch_add(1, std::memory_order_relaxed);
		}

		break;
	}
	default:
		break;
	}

	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static bool dz_register_rawinput(HWND hwnd)
{
	RAWINPUTDEVICE rid[2]{};

	rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
	rid[0].usUsage = HID_USAGE_GENERIC_MOUSE;
	rid[0].dwFlags = RIDEV_INPUTSINK;
	rid[0].hwndTarget = hwnd;

	rid[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
	rid[1].usUsage = HID_USAGE_GENERIC_KEYBOARD;
	rid[1].dwFlags = RIDEV_INPUTSINK;
	rid[1].hwndTarget = hwnd;

	return RegisterRawInputDevices(rid, 2, sizeof(rid[0])) == TRUE;
}

static HWND dz_create_hidden_window(dz_source_data *d)
{
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = dz_wndproc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = kWndClass;

	static std::atomic<bool> registered{false};
	if (!registered.exchange(true)) {
		RegisterClassExW(&wc);
	}

	HWND hwnd = CreateWindowExW(0, kWndClass, L"DZ Analisador de Entrada Oculto", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
				    nullptr, nullptr, GetModuleHandleW(nullptr), d);

	if (!hwnd)
		return nullptr;

	ShowWindow(hwnd, SW_HIDE);
	if (!dz_register_rawinput(hwnd)) {
		DestroyWindow(hwnd);
		return nullptr;
	}

	return hwnd;
}

// ------------------------------------------------------------
// Auxiliares de desenho (efeito sólido)
static void dz_draw_rect(gs_effect_t *solid, float x, float y, float w, float h, const vec4 &c)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(solid, "color");
	if (!p)
		return;

	gs_effect_set_vec4(p, &c);

	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);

	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(nullptr, 0, (uint32_t)w, (uint32_t)h);
	}

	gs_matrix_pop();
}

// Fonte bitmap mínima (5x7) para letras e números
static uint8_t glyph_5x7(char ch, int row)
{
	// Cada glifo: 7 linhas de 5 bits (bit mais significativo à esquerda)
	// Retorna a máscara da linha (0..6), bits em 0..4.
	switch (ch) {
	case 'A': {
		static const uint8_t g[7] = {0b00100, 0b01010, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001};
		return g[row];
	}
	case 'B': {
		static const uint8_t g[7] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110};
		return g[row];
	}
	case 'C': {
		static const uint8_t g[7] = {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110};
		return g[row];
	}
	case 'D': {
		static const uint8_t g[7] = {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110};
		return g[row];
	}
	case 'E': {
		static const uint8_t g[7] = {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111};
		return g[row];
	}
	case 'F': {
		static const uint8_t g[7] = {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000};
		return g[row];
	}
	case 'G': {
		static const uint8_t g[7] = {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01110};
		return g[row];
	}
	case 'H': {
		static const uint8_t g[7] = {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001};
		return g[row];
	}
	case 'I': {
		static const uint8_t g[7] = {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110};
		return g[row];
	}
	case 'J': {
		static const uint8_t g[7] = {0b00111, 0b00010, 0b00010, 0b00010, 0b10010, 0b10010, 0b01100};
		return g[row];
	}
	case 'K': {
		static const uint8_t g[7] = {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001};
		return g[row];
	}
	case 'L': {
		static const uint8_t g[7] = {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111};
		return g[row];
	}
	case 'M': {
		static const uint8_t g[7] = {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001};
		return g[row];
	}
	case 'N': {
		static const uint8_t g[7] = {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001};
		return g[row];
	}
	case 'O': {
		static const uint8_t g[7] = {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
		return g[row];
	}
	case 'P': {
		static const uint8_t g[7] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000};
		return g[row];
	}
	case 'Q': {
		static const uint8_t g[7] = {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101};
		return g[row];
	}
	case 'R': {
		static const uint8_t g[7] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001};
		return g[row];
	}
	case 'S': {
		static const uint8_t g[7] = {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110};
		return g[row];
	}
	case 'T': {
		static const uint8_t g[7] = {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100};
		return g[row];
	}
	case 'U': {
		static const uint8_t g[7] = {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
		return g[row];
	}
	case 'V': {
		static const uint8_t g[7] = {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100};
		return g[row];
	}
	case 'W': {
		static const uint8_t g[7] = {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010};
		return g[row];
	}
	case 'X': {
		static const uint8_t g[7] = {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001};
		return g[row];
	}
	case 'Y': {
		static const uint8_t g[7] = {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100};
		return g[row];
	}
	case 'Z': {
		static const uint8_t g[7] = {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111};
		return g[row];
	}
	case '0': {
		static const uint8_t g[7] = {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110};
		return g[row];
	}
	case '1': {
		static const uint8_t g[7] = {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110};
		return g[row];
	}
	case '2': {
		static const uint8_t g[7] = {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111};
		return g[row];
	}
	case '3': {
		static const uint8_t g[7] = {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110};
		return g[row];
	}
	case '4': {
		static const uint8_t g[7] = {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010};
		return g[row];
	}
	case '5': {
		static const uint8_t g[7] = {0b11111, 0b10000, 0b10000, 0b11110, 0b00001, 0b00001, 0b11110};
		return g[row];
	}
	
	case '6': {
		static const uint8_t g[7] = {0b01110, 0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110};
		return g[row];
	}
	case '7': {
		static const uint8_t g[7] = {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000};
		return g[row];
	}
	case '8': {
		static const uint8_t g[7] = {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110};
		return g[row];
	}
	case '9': {
		static const uint8_t g[7] = {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b01110};
		return g[row];
	}
	default:
		return 0;
	}
}

static void dz_draw_text_5x7(gs_effect_t *solid, float x, float y, const char *text, float scale, const vec4 &color)
{
	if (!text || !*text)
		return;

	const float px = std::max(1.0f, std::floor(scale));
	const float cell = px;

	float pen_x = x;
	for (const char *p = text; *p; ++p) {
		const char ch = *p;
		if (ch == ' ') {
			pen_x += 6.0f * cell;
			continue;
		}

		for (int r = 0; r < 7; r++) {
			const uint8_t bits = glyph_5x7(ch, r);
			for (int c = 0; c < 5; c++) {
				if (bits & (1u << (4 - c))) {
					dz_draw_rect(solid, pen_x + c * cell, y + r * cell, cell, cell, color);
				}
			}
		}
		pen_x += 6.0f * cell; // 5 + 1 espaço
	}
}

// ------------------------------------------------------------
// Fonte OBS
static uint16_t dz_obter_vkey(obs_data_t *settings, const char *nome, uint16_t padrao)
{
	const int valor = (int)obs_data_get_int(settings, nome);
	return valor != 0 ? (uint16_t)valor : padrao;
}

static void dz_preencher_lista_teclas(obs_property_t *list)
{
	for (const auto &opt : kKeyOptions)
		obs_property_list_add_int(list, opt.nome, opt.vkey);
}

static bool dz_ao_modificar_tecla(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	const char *prop_name = obs_property_name(property);
	const uint16_t vkey = (uint16_t)obs_data_get_int(settings, prop_name);
	const char *grupo_id = nullptr;

	if (strcmp(prop_name, "row_w_key") == 0)
		grupo_id = "row_w_group";
	else if (strcmp(prop_name, "row_s_key") == 0)
		grupo_id = "row_s_group";
	else if (strcmp(prop_name, "row_a_key") == 0)
		grupo_id = "row_a_group";
	else if (strcmp(prop_name, "row_d_key") == 0)
		grupo_id = "row_d_group";

	if (!grupo_id)
		return true;

	obs_property_t *grupo = obs_properties_get(props, grupo_id);
	if (!grupo)
		return true;

	std::string titulo = dz_titulo_tecla(vkey);
	obs_property_set_description(grupo, titulo.c_str());
	return true;
}
static const char *dz_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Analisador de Entrada DZ";
}

static void *dz_source_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);

	auto *d = new dz_source_data();

	const int w = (int)obs_data_get_int(settings, "width");
	const int h = (int)obs_data_get_int(settings, "height");
	if (w > 0)
		d->width = (uint32_t)w;
	if (h > 0)
		d->height = (uint32_t)h;

	d->bg_alpha = (float)obs_data_get_double(settings, "bg_alpha");
	d->bg_alpha = std::clamp(d->bg_alpha, 0.0f, 1.0f);

	d->bg_color = (uint32_t)obs_data_get_int(settings, "bg_color");
	d->key_color[ROW_W] = (uint32_t)obs_data_get_int(settings, "color_w");
	d->key_color[ROW_S] = (uint32_t)obs_data_get_int(settings, "color_s");
	d->key_color[ROW_A] = (uint32_t)obs_data_get_int(settings, "color_a");
	d->key_color[ROW_D] = (uint32_t)obs_data_get_int(settings, "color_d");
	d->row_key_vkey[ROW_W] = dz_obter_vkey(settings, "row_w_key", 'W');
	d->row_key_vkey[ROW_S] = dz_obter_vkey(settings, "row_s_key", 'S');
	d->row_key_vkey[ROW_A] = dz_obter_vkey(settings, "row_a_key", 'A');
	d->row_key_vkey[ROW_D] = dz_obter_vkey(settings, "row_d_key", 'D');
	d->row_enabled[ROW_W] = obs_data_get_bool(settings, "row_w_enabled");
	d->row_enabled[ROW_S] = obs_data_get_bool(settings, "row_s_enabled");
	d->row_enabled[ROW_A] = obs_data_get_bool(settings, "row_a_enabled");
	d->row_enabled[ROW_D] = obs_data_get_bool(settings, "row_d_enabled");

	obs_enter_graphics();
	d->solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	obs_leave_graphics();

	d->hwnd = dz_create_hidden_window(d);

	blog(LOG_INFO, "[dz-input-analyzer] criar: %ux%u solido=%p hwnd=%p", d->width, d->height, d->solid, d->hwnd);
	return d;
}

static void dz_source_destroy(void *data)
{
	auto *d = (dz_source_data *)data;
	if (!d)
		return;

	blog(LOG_INFO, "[dz-input-analyzer] destruir");

	if (d->hwnd) {
		DestroyWindow(d->hwnd);
		d->hwnd = nullptr;
	}

	obs_enter_graphics();
	d->solid = nullptr;
	obs_leave_graphics();

	delete d;
}

static uint32_t dz_source_get_width(void *data)
{
	auto *d = (dz_source_data *)data;
	return d ? d->width : 0;
}

static uint32_t dz_source_get_height(void *data)
{
	auto *d = (dz_source_data *)data;
	return d ? d->height : 0;
}

static void dz_source_defaults(obs_data_t *settings)
{
	// Manter em sincronia com os padrões de dz_source_data
	obs_data_set_default_int(settings, "width", 1500);
	obs_data_set_default_int(settings, "height", 520);

	obs_data_set_default_double(settings, "bg_alpha", 0.55);

	// Cores são COLORREF (BGR): 0x00BBGGRR
	obs_data_set_default_int(settings, "bg_color", 0x000000);
	obs_data_set_default_int(settings, "color_w", 0x005dc8f3); // #f3c85d
	obs_data_set_default_int(settings, "color_s", 0x009cff9c); // #9cff9c
	obs_data_set_default_int(settings, "color_a", 0x003f3fcf); // #cf3f3f
	obs_data_set_default_int(settings, "color_d", 0x00c8a00a); // #0aa0c8

	obs_data_set_default_int(settings, "row_w_key", 'W');
	obs_data_set_default_int(settings, "row_s_key", 'S');
	obs_data_set_default_int(settings, "row_a_key", 'A');
	obs_data_set_default_int(settings, "row_d_key", 'D');

	obs_data_set_default_bool(settings, "row_w_enabled", true);
	obs_data_set_default_bool(settings, "row_s_enabled", true);
	obs_data_set_default_bool(settings, "row_a_enabled", true);
	obs_data_set_default_bool(settings, "row_d_enabled", true);
}

static obs_properties_t *dz_source_properties(void *data)
{
	auto *d = (dz_source_data *)data;
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_int(p, "width", "Largura", 16, 16384, 1);
	obs_properties_add_int(p, "height", "Altura", 16, 16384, 1);
	obs_properties_add_float_slider(p, "bg_alpha", "Opacidade do fundo", 0.0, 1.0, 0.01);

	obs_properties_add_color(p, "bg_color", "Cor do fundo");

	const uint16_t vkey_w = d ? d->row_key_vkey[ROW_W] : (uint16_t)'W';
	const uint16_t vkey_s = d ? d->row_key_vkey[ROW_S] : (uint16_t)'S';
	const uint16_t vkey_a = d ? d->row_key_vkey[ROW_A] : (uint16_t)'A';
	const uint16_t vkey_d = d ? d->row_key_vkey[ROW_D] : (uint16_t)'D';

	{
		obs_properties_t *grupo = obs_properties_create();
		obs_property_t *lista = obs_properties_add_list(grupo, "row_w_key", "Tecla monitorada",
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		dz_preencher_lista_teclas(lista);
		obs_property_set_modified_callback(lista, dz_ao_modificar_tecla);
		obs_properties_add_color(grupo, "color_w", "Cor da linha");
		obs_properties_add_bool(grupo, "row_w_enabled", "Mostrar linha");
		obs_properties_add_group(p, "row_w_group", dz_titulo_tecla(vkey_w).c_str(), OBS_GROUP_NORMAL, grupo);
	}

	{
		obs_properties_t *grupo = obs_properties_create();
		obs_property_t *lista = obs_properties_add_list(grupo, "row_s_key", "Tecla monitorada",
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		dz_preencher_lista_teclas(lista);
		obs_property_set_modified_callback(lista, dz_ao_modificar_tecla);
		obs_properties_add_color(grupo, "color_s", "Cor da linha");
		obs_properties_add_bool(grupo, "row_s_enabled", "Mostrar linha");
		obs_properties_add_group(p, "row_s_group", dz_titulo_tecla(vkey_s).c_str(), OBS_GROUP_NORMAL, grupo);
	}

	{
		obs_properties_t *grupo = obs_properties_create();
		obs_property_t *lista = obs_properties_add_list(grupo, "row_a_key", "Tecla monitorada",
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		dz_preencher_lista_teclas(lista);
		obs_property_set_modified_callback(lista, dz_ao_modificar_tecla);
		obs_properties_add_color(grupo, "color_a", "Cor da linha");
		obs_properties_add_bool(grupo, "row_a_enabled", "Mostrar linha");
		obs_properties_add_group(p, "row_a_group", dz_titulo_tecla(vkey_a).c_str(), OBS_GROUP_NORMAL, grupo);
	}

	{
		obs_properties_t *grupo = obs_properties_create();
		obs_property_t *lista = obs_properties_add_list(grupo, "row_d_key", "Tecla monitorada",
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		dz_preencher_lista_teclas(lista);
		obs_property_set_modified_callback(lista, dz_ao_modificar_tecla);
		obs_properties_add_color(grupo, "color_d", "Cor da linha");
		obs_properties_add_bool(grupo, "row_d_enabled", "Mostrar linha");
		obs_properties_add_group(p, "row_d_group", dz_titulo_tecla(vkey_d).c_str(), OBS_GROUP_NORMAL, grupo);
	}
	return p;
}

static void dz_source_update(void *data, obs_data_t *settings)
{
	auto *d = (dz_source_data *)data;
	if (!d)
		return;

	const int w = (int)obs_data_get_int(settings, "width");
	const int h = (int)obs_data_get_int(settings, "height");
	if (w > 0)
		d->width = (uint32_t)w;
	if (h > 0)
		d->height = (uint32_t)h;

	d->bg_alpha = (float)obs_data_get_double(settings, "bg_alpha");
	d->bg_alpha = std::clamp(d->bg_alpha, 0.0f, 1.0f);

	d->bg_color = (uint32_t)obs_data_get_int(settings, "bg_color");
	d->key_color[ROW_W] = (uint32_t)obs_data_get_int(settings, "color_w");
	d->key_color[ROW_S] = (uint32_t)obs_data_get_int(settings, "color_s");
	d->key_color[ROW_A] = (uint32_t)obs_data_get_int(settings, "color_a");
	d->key_color[ROW_D] = (uint32_t)obs_data_get_int(settings, "color_d");

	d->row_key_vkey[ROW_W] = dz_obter_vkey(settings, "row_w_key", 'W');
	d->row_key_vkey[ROW_S] = dz_obter_vkey(settings, "row_s_key", 'S');
	d->row_key_vkey[ROW_A] = dz_obter_vkey(settings, "row_a_key", 'A');
	d->row_key_vkey[ROW_D] = dz_obter_vkey(settings, "row_d_key", 'D');

	d->row_enabled[ROW_W] = obs_data_get_bool(settings, "row_w_enabled");
	d->row_enabled[ROW_S] = obs_data_get_bool(settings, "row_s_enabled");
	d->row_enabled[ROW_A] = obs_data_get_bool(settings, "row_a_enabled");
	d->row_enabled[ROW_D] = obs_data_get_bool(settings, "row_d_enabled");

	for (int i = 0; i < ROW_COUNT; i++)
		d->st.key_down[i].store(0, std::memory_order_relaxed);

	blog(LOG_INFO, "[dz-input-analyzer] atualizar: %ux%u opacidade=%.2f cor_fundo=%06x W=%06x S=%06x A=%06x D=%06x",
		d->width, d->height, d->bg_alpha,
		(unsigned)(d->bg_color & 0xFFFFFF),
		(unsigned)(d->key_color[ROW_W] & 0xFFFFFF),
		(unsigned)(d->key_color[ROW_S] & 0xFFFFFF),
		(unsigned)(d->key_color[ROW_A] & 0xFFFFFF),
		(unsigned)(d->key_color[ROW_D] & 0xFFFFFF));
}

// Cores do movv.html
static vec4 dz_col_rgba(float r, float g, float b, float a)
{
	vec4 c;
	vec4_set(&c, r, g, b, a);
	return c;
}

// A propriedade de cor do OBS usa COLORREF do Windows (BGR): 0x00BBGGRR
static inline vec4 dz_col_from_obs_bgr(uint32_t bgr, float a)
{
	const float r = (float)((bgr) & 0xFF) / 255.0f;
	const float g = (float)((bgr >> 8) & 0xFF) / 255.0f;
	const float b = (float)((bgr >> 16) & 0xFF) / 255.0f;
	return dz_col_rgba(r, g, b, a);
}

static vec4 row_color(const dz_source_data *d, int row, float a)
{
	if (!d)
		return dz_col_rgba(1.0f, 1.0f, 1.0f, a);

	const int idx = std::clamp(row, 0, ROW_COUNT - 1);
	return dz_col_from_obs_bgr(d->key_color[idx], a);
}

// Mantém somente os últimos 30s, como no movv.html
static void dz_cleanup_history(dz_source_data *d, int64_t t_now_ms)
{
	const int64_t keep_after = t_now_ms - 30000;

	while (!d->clicks.empty() && d->clicks.front().time_ms < keep_after)
		d->clicks.pop_front();

	while (!d->segments.empty()) {
		auto &s0 = d->segments.front();
		const int64_t end0 = (s0.end_ms < 0) ? t_now_ms : s0.end_ms;
		if (end0 >= keep_after)
			break;
		d->segments.pop_front();
	}
}

static void dz_source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	auto *d = (dz_source_data *)data;
	if (!d || !d->solid)
		return;

	d->frame_counter++;

	const float W = (float)d->width;
	const float H = (float)d->height;

	gs_effect_t *solid = d->solid;

	gs_reset_blend_state();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	// IMPORTANTE:
	// Não sobrescreva viewport/projeção aqui.
	// Deixe o OBS controlar todas as transformações (mover/escalar/rotacionar)
	// para manter o conteúdo preso ao retângulo da fonte.

	// Fundo (cor simples)
	vec4 bg = dz_col_from_obs_bgr(d->bg_color, d->bg_alpha);
	dz_draw_rect(solid, 0.0f, 0.0f, W, H, bg);

	// Layout copiado do draw() do movv.html
	const float leftPad = 70.0f;
	const float rightPad = 20.0f;
	const float topPad = 18.0f;
	const float bottomPad = 55.0f;

	const float timelineX0 = leftPad;
	const float timelineX1 = W - rightPad;
	const float timelineW = timelineX1 - timelineX0;

	const float rowsAreaH = H - topPad - bottomPad;
	const float rowGap = 20.0f;
	int visible_rows = 0;
	for (int i = 0; i < ROW_COUNT; i++) {
		if (d->row_enabled[i])
			visible_rows++;
	}

	const float rowH = (visible_rows > 0)
			       ? std::floor((rowsAreaH - rowGap * (visible_rows - 1)) / (float)visible_rows)
			       : 0.0f;

	float rowYs[ROW_COUNT];
	for (int i = 0; i < ROW_COUNT; i++)
		rowYs[i] = -1.0f;

	if (visible_rows > 0) {
		int row_index = 0;
		for (int i = 0; i < ROW_COUNT; i++) {
			if (!d->row_enabled[i])
				continue;
			rowYs[i] = topPad + row_index * (rowH + rowGap);
			row_index++;
		}
	}

	// Janela de tempo (móvel)
	const int64_t tNow = now_ms();
	const int64_t WINDOW_MS = 5000;
	const int64_t t0 = tNow - WINDOW_MS;
	const int64_t t1 = tNow;

	auto xOf = [&](int64_t t) -> float {
		const double denom = (double)(t1 - t0);
		if (denom <= 0.0)
			return timelineX0;
		const double u = (double)(t - t0) / denom;
		return timelineX0 + (float)(u * (double)timelineW);
	};

	auto clampf = [&](float v, float a, float b) -> float {
		return std::max(a, std::min(b, v));
	};

	// Linhas verticais da grade em 0..5s
	vec4 grid = dz_col_rgba(0.160784f, 0.160784f, 0.160784f, 1.0f); // #292929
	const float axisY = H - bottomPad + 22.0f;
	const float axisY2 = axisY + 2.0f; // espessura da base
	for (int i = 0; i <= 5; i++) {
		const float x = timelineX0 + ((float)i * 1000.0f / (float)WINDOW_MS) * timelineW;
		const float y0 = topPad - 6.0f;
		const float h = std::max(2.0f, axisY2 - y0);
		dz_draw_rect(solid, x, y0, 2.0f, h, grid);
	}

	
	// Rótulos das linhas usando fonte bitmap
	if (visible_rows > 0) {
		vec4 text = dz_col_rgba(1.0f, 1.0f, 1.0f, 0.92f);
		for (int i = 0; i < ROW_COUNT; i++) {
			if (!d->row_enabled[i])
				continue;
			const char *label = dz_nome_tecla(d->row_key_vkey[i]);
			const size_t label_len = strlen(label);
			float scale = 4.0f;
			if (label_len > 10)
				scale = 3.0f;
			if (label_len > 16)
				scale = 2.0f;
			const float yMid = rowYs[i] + rowH * 0.5f;
			// Centraliza o bloco 5x7 verticalmente em yMid
			const float glyphH = 7.0f * std::floor(scale);
			const float y = yMid - glyphH * 0.5f;
			dz_draw_text_5x7(solid, 22.0f, y, label, scale, text);
		}
	}

	// Segmentos da tecla (altura 60% de rowH, cantos retos)
	if (visible_rows > 0) {
		for (const auto &seg : d->segments) {
			if (!d->row_enabled[seg.row])
				continue;
		const int64_t end = (seg.end_ms < 0) ? tNow : seg.end_ms;

		if (end < t0 || seg.start_ms > t1)
			continue;

		float x0s = clampf(xOf(seg.start_ms), timelineX0, timelineX1);
		float x1s = clampf(xOf(end), timelineX0, timelineX1);

		float w = std::max(2.0f, x1s - x0s);
		float h = std::max(2.0f, std::round(rowH * 0.60f));
		float y = rowYs[seg.row] + std::round((rowH - h) * 0.5f);

			vec4 c = row_color(d, seg.row, 0.95f);
			dz_draw_rect(solid, x0s, y, w, h, c);
		}
	}

	// Marcadores de clique + números de delta
	if (visible_rows > 0) {
		for (const auto &c : d->clicks) {
			if (!d->row_enabled[c.row])
				continue;
			if (c.time_ms < t0 || c.time_ms > t1)
				continue;

			const float x = xOf(c.time_ms);

			// Cor e altura são definidos pela última tecla pressionada antes do clique (c.row).
			// Uma variável controla tanto a linha do clique quanto a cor do número de delta.
			vec4 clickCol = row_color(d, c.row, 0.90f);

			// Linha do clique: começa no TOPO da linha da última tecla e termina na base.
			const float y0 = rowYs[c.row];
			const float h = std::max(2.0f, axisY2 - y0);
			dz_draw_rect(solid, x, y0, 2.0f, h, clickCol);

			// Número próximo da linha (mesma cor da linha do clique)
			const float scale = 3.0f;

			char buf[16]{};
			_snprintf_s(buf, _TRUNCATE, "%d", c.delta_ms);

			const float yText = rowYs[c.row] - 6.0f;
			dz_draw_text_5x7(solid, x + 6.0f, yText + 0.1f, buf, scale, clickCol);
		}
	}

	// Linha do eixo de TEMPO + marcas + rótulos 0s..5s
	{
		const float axisY = H - bottomPad + 22.0f;

		// Linha base do eixo: #292929
		vec4 axis = dz_col_rgba(0.1608f, 0.1608f, 0.1608f, 1.0f);
		dz_draw_rect(solid, timelineX0, axisY, timelineW, 2.0f, axis);

		// Cor das marcas/grade: #292929 (somente 6 linhas verticais)
		vec4 grid = dz_col_rgba(0.1608f, 0.1608f, 0.1608f, 1.0f);

		// Cor dos rótulos (0s..5s)
		vec4 tcol = dz_col_rgba(0.1608f, 0.1608f, 0.1608f, 1.0f);

		for (int i = 0; i <= 5; i++) {
			const float x = timelineX0 + ((float)i * 1000.0f / (float)WINDOW_MS) * timelineW;
			dz_draw_rect(solid, x, axisY, 2.0f, 12.0f, grid);

			char lab[4]{};
			lab[0] = (char)('0' + i);
			lab[1] = 'S';
			lab[2] = 0;

			dz_draw_text_5x7(solid, x - 10.0f, axisY + 10.0f, lab, 2.28f, tcol);
		}
	}


	// Limpeza do histórico como no movv.html (mantém 30s)
	dz_cleanup_history(d, tNow);

	// O OBS controla viewport/projeção.
}

static obs_source_info dz_source_info;

bool obs_module_load(void)
{
	memset(&dz_source_info, 0, sizeof(dz_source_info));

	dz_source_info.id = "dz_input_analyzer";
	dz_source_info.type = OBS_SOURCE_TYPE_INPUT;
	dz_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;

	dz_source_info.get_name = dz_source_get_name;
	dz_source_info.create = dz_source_create;
	dz_source_info.destroy = dz_source_destroy;
	dz_source_info.update = dz_source_update;
	dz_source_info.get_properties = dz_source_properties;
	dz_source_info.get_defaults = dz_source_defaults;

	dz_source_info.get_width = dz_source_get_width;
	dz_source_info.get_height = dz_source_get_height;
	dz_source_info.video_render = dz_source_render;

	obs_register_source(&dz_source_info);
	blog(LOG_INFO, "[dz-input-analyzer] fonte registrada (linha do tempo)");
	return true;
}

const char *obs_module_name(void)
{
	return "Analisador de Entrada DZ";
}
