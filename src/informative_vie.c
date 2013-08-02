#include "pebble_os.h"
#include "pebble_app.h"
#include "pebble_fonts.h"
#include "stdlib.h"
#include "string.h"
#include "config.h"
#include "my_math.h"
#include "suncalc.h"
#include "http.h"
#include "util.h"
#include "link_monitor.h"

#define MY_UUID { 0xEE, 0x27, 0xFA, 0x56, 0x21, 0x65, 0x4D, 0x02, 0xA8, 0xAE, 0xB1, 0xD4, 0xD2, 0x3B, 0x9F, 0x50 }

PBL_APP_INFO(MY_UUID, "Informative Vie", "whitecat", 0, 8, /* App major/minor version */
RESOURCE_ID_IMAGE_MENU_ICON, APP_INFO_WATCH_FACE);

Window window;
//TextLayer cwLayer; 					// The calendar week
TextLayer text_sunrise_layer;
TextLayer text_sunrise_title_layer;
TextLayer text_sunset_layer;
TextLayer text_sunset_title_layer;
TextLayer text_temperature_layer;
TextLayer DayOfWeekLayer;
TextLayer timeLayer;
TextLayer dateLayer;
//TextLayer calls_layer;   			/* layer for Phone Calls info */
//TextLayer sms_layer;   				/* layer for SMS info */
TextLayer debug_layer; /* layer for DEBUG info */

#define LINE_BUFFER_SIZE 50
static char str_month[LINE_BUFFER_SIZE];
static char str_year[LINE_BUFFER_SIZE];
static char str_time[LINE_BUFFER_SIZE];
static char str_date[LINE_BUFFER_SIZE];

static int our_latitude, our_longitude, our_timezone = 99;
static bool located = false;
static bool calculated_sunset_sunrise = false;
static bool temperature_set = false;

GFont font_temperature; /* font for Temperature */
GFont font_time; /* font for Time */
GFont font_date; /* font for Date */

#define TOTAL_WEATHER_IMAGES 1
BmpContainer weather_images[TOTAL_WEATHER_IMAGES];

const int WEATHER_IMAGE_RESOURCE_IDS[] = { RESOURCE_ID_IMAGE_CLEAR_DAY,
		RESOURCE_ID_IMAGE_CLEAR_NIGHT, RESOURCE_ID_IMAGE_RAIN,
		RESOURCE_ID_IMAGE_SNOW, RESOURCE_ID_IMAGE_SLEET, RESOURCE_ID_IMAGE_WIND,
		RESOURCE_ID_IMAGE_FOG, RESOURCE_ID_IMAGE_CLOUDY,
		RESOURCE_ID_IMAGE_PARTLY_CLOUDY_DAY,
		RESOURCE_ID_IMAGE_PARTLY_CLOUDY_NIGHT, RESOURCE_ID_IMAGE_NO_WEATHER };

//#define TOTAL_TIME_DIGITS 4
//BmpContainer time_digits_images[TOTAL_TIME_DIGITS];

void set_container_image(BmpContainer *bmp_container, const int resource_id,
		GPoint origin) {
	layer_remove_from_parent(&bmp_container->layer.layer);
	bmp_deinit_container(bmp_container);

	bmp_init_container(resource_id, bmp_container);

	GRect frame = layer_get_frame(&bmp_container->layer.layer);
	frame.origin.x = origin.x;
	frame.origin.y = origin.y;
	layer_set_frame(&bmp_container->layer.layer, frame);

	layer_add_child(&window.layer, &bmp_container->layer.layer);
}

unsigned short get_display_hour(unsigned short hour) {
	if (clock_is_24h_style()) {
		return hour;
	}

	unsigned short display_hour = hour % 12;

	// Converts "0" to "12"
	return display_hour ? display_hour : 12;
}

int moon_phase(int y, int m, int d) {
	/*
	 calculates the moon phase (0-7), accurate to 1 segment.
	 0 = > new moon.
	 4 => full moon.
	 */
	int c, e;
	double jd;
	int b;

	if (m < 3) {
		y--;
		m += 12;
	}
	++m;
	c = 365.25 * y;
	e = 30.6 * m;
	jd = c + e + d - 694039.09; /* jd is total days elapsed */
	jd /= 29.53; /* divide by the moon cycle (29.53 days) */
	b = jd; /* int(jd) -> b, take integer part of jd */
	jd -= b; /* subtract integer part to leave fractional part of original jd */
	b = jd * 8 + 0.5; /* scale fraction from 0-8 and round by adding 0.5 */
	b = b & 7; /* 0 and 8 are the same so turn 8 into 0 */
	return b;
}

void adjustTimezone(float* time) {
	*time += our_timezone;
	if (*time > 24)
		*time -= 24;
	if (*time < 0)
		*time += 24;
}

void updateSunsetSunrise() {
	// Calculating Sunrise/sunset with courtesy of Michael Ehrmann
	// https://github.com/mehrmann/pebble-sunclock
	static char sunrise_text[] = "00:00";
	static char sunset_text[] = "00:00";

	PblTm pblTime;
	get_time(&pblTime);

	char *time_format;

	if (clock_is_24h_style()) {
		time_format = "%R";
	} else {
		time_format = "%I:%M";
	}

	float sunriseTime = calcSunRise(pblTime.tm_year, pblTime.tm_mon + 1,
			pblTime.tm_mday, our_latitude / 10000, our_longitude / 10000,
			91.0f);
	float sunsetTime = calcSunSet(pblTime.tm_year, pblTime.tm_mon + 1,
			pblTime.tm_mday, our_latitude / 10000, our_longitude / 10000,
			91.0f);
	adjustTimezone(&sunriseTime);
	adjustTimezone(&sunsetTime);

	if (!pblTime.tm_isdst) {
		sunriseTime += 1;
		sunsetTime += 1;
	}

	pblTime.tm_min = (int) (60 * (sunriseTime - ((int) (sunriseTime))));
	pblTime.tm_hour = (int) sunriseTime;
	string_format_time(sunrise_text, sizeof(sunrise_text), time_format,
			&pblTime);
	text_layer_set_text(&text_sunrise_layer, sunrise_text);

	pblTime.tm_min = (int) (60 * (sunsetTime - ((int) (sunsetTime))));
	pblTime.tm_hour = (int) sunsetTime;
	string_format_time(sunset_text, sizeof(sunset_text), time_format, &pblTime);
	text_layer_set_text(&text_sunset_layer, sunset_text);
}

unsigned short the_last_hour = 25;

void request_weather();

void display_counters(TextLayer *dataLayer, struct Data d, int infoType) {

	static char temp_text[5];

	if (d.link_status != LinkStatusOK) {
		memcpy(temp_text, "?", 1);
	} else {
		if (infoType == 1) {
			if (d.missed) {
				memcpy(temp_text, itoa(d.missed), 4);
			} else {
				memcpy(temp_text, itoa(0), 4);
			}
		} else if (infoType == 2) {
			if (d.unread) {
				memcpy(temp_text, itoa(d.unread), 4);
			} else {
				memcpy(temp_text, itoa(0), 4);
			}
		}
	}

	text_layer_set_text(dataLayer, temp_text);
}

void failed(int32_t cookie, int http_status, void* context) {

	if ((cookie == 0 || cookie == WEATHER_HTTP_COOKIE) && !temperature_set) {
		set_container_image(&weather_images[0], WEATHER_IMAGE_RESOURCE_IDS[10],
				GPoint(12, 120));
		text_layer_set_text(&text_temperature_layer, "---°");
	}

	//link_monitor_handle_failure(http_status);

	//Re-request the location and subsequently weather on next minute tick
	//located = false;
}

void success(int32_t cookie, int http_status, DictionaryIterator* received,
		void* context) {

	if (cookie != WEATHER_HTTP_COOKIE)
		return;

	Tuple* icon_tuple = dict_find(received, WEATHER_KEY_ICON);
	if (icon_tuple) {
		int icon = icon_tuple->value->int8;
		if (icon >= 0 && icon < 10) {
			set_container_image(&weather_images[0],
					WEATHER_IMAGE_RESOURCE_IDS[icon], GPoint(12, 120)); // ---------- Weather Image
		} else {
			set_container_image(&weather_images[0],
					WEATHER_IMAGE_RESOURCE_IDS[10], GPoint(12, 120));
		}
	}

	Tuple* temperature_tuple = dict_find(received, WEATHER_KEY_TEMPERATURE);
	if (temperature_tuple) {

		static char temp_text[5];
		memcpy(temp_text, itoa(temperature_tuple->value->int16), 4);
		int degree_pos = strlen(temp_text);
		memcpy(&temp_text[degree_pos], "°", 3);
		text_layer_set_text(&text_temperature_layer, temp_text);
		temperature_set = true;
	}

	link_monitor_handle_success(&data);
	//display_counters(&calls_layer, data, 1);
	//display_counters(&sms_layer, data, 2);
}

void location(float latitude, float longitude, float altitude, float accuracy,
		void* context) {
	// Fix the floats
	our_latitude = latitude * 10000;
	our_longitude = longitude * 10000;
	located = true;
	request_weather();
}

void reconnect(void* context) {
	located = false;
	request_weather();
}

bool read_state_data(DictionaryIterator* received, struct Data* d) {
	(void) d;
	bool has_data = false;
	Tuple* tuple = dict_read_first(received);
	if (!tuple)
		return false;
	do {
		switch (tuple->key) {
		case TUPLE_MISSED_CALLS:
			d->missed = tuple->value->uint8;

			static char temp_calls[5];
			memcpy(temp_calls, itoa(tuple->value->uint8), 4);
//				text_layer_set_text(&calls_layer, temp_calls);

			has_data = true;
			break;
		case TUPLE_UNREAD_SMS:
			d->unread = tuple->value->uint8;

			static char temp_sms[5];
			memcpy(temp_sms, itoa(tuple->value->uint8), 4);
//				text_layer_set_text(&sms_layer, temp_sms);

			has_data = true;
			break;
		}
	} while ((tuple = dict_read_next(received)));
	return has_data;
}

void app_received_msg(DictionaryIterator* received, void* context) {
	link_monitor_handle_success(&data);
//	if(read_state_data(received, &data))
//	{
	//display_counters(&calls_layer, data, 1);
	//display_counters(&sms_layer, data, 2);
	if (!located) {
		request_weather();
	}
//	}
}
static void app_send_failed(DictionaryIterator* failed, AppMessageResult reason,
		void* context) {
	link_monitor_handle_failure(reason, &data);
	//display_counters(&calls_layer, data, 1);
	//display_counters(&sms_layer, data, 2);
}

bool register_callbacks() {
	if (callbacks_registered) {
		if (app_message_deregister_callbacks(&app_callbacks) == APP_MSG_OK)
			callbacks_registered = false;
	}
	if (!callbacks_registered) {
		app_callbacks =
				(AppMessageCallbacksNode) {
							.callbacks = {.in_received = app_received_msg, .out_failed = app_send_failed}};
						if (app_message_register_callbacks(&app_callbacks) == APP_MSG_OK) {
							callbacks_registered = true;
						}
					}
	return callbacks_registered;
}

void receivedtime(int32_t utc_offset_seconds, bool is_dst, uint32_t unixtime,
		const char* tz_name, void* context) {
	our_timezone = (utc_offset_seconds / 3600);
	if (is_dst) {
		our_timezone--;
	}

	if (located && our_timezone != 99 && !calculated_sunset_sunrise) {
		updateSunsetSunrise();
		calculated_sunset_sunrise = true;
	}
}

void update_display(PblTm *current_time) {

	unsigned short display_hour = get_display_hour(current_time->tm_hour);
//This is how we will do timezones when it is working
//	string_format_time(str_time, sizeof(str_time), "%H:%M %Z", current_time);
	string_format_time(str_time, sizeof(str_time), "%H:%M ", current_time);

	text_layer_set_text(&timeLayer, str_time);

	if (the_last_hour != display_hour) {

		//Get month and year from current time.
		string_format_time(str_month, sizeof(str_month), "%b ", current_time);
		string_format_time(str_year, sizeof(str_year), ", %Y", current_time);

		//Get day from current time.
		unsigned short i = get_display_hour(current_time->tm_mday);

		char num_rep[LINE_BUFFER_SIZE];
		snprintf(num_rep, LINE_BUFFER_SIZE, "%d", i);

		strcpy(str_date, str_month);
		strcat(str_date, num_rep);
		//Set the ending of the number of date.
		int j = i % 10;
		if (j == 1 && i != 11) {

			strcat(str_date, "st");
		} else if (j == 2 && i != 12) {
			strcat(str_date, "nd");
		} else if (j == 3 && i != 13) {
			strcat(str_date, "rd");
		} else
			strcat(str_date, "th");

		strcat(str_date, str_year);

		text_layer_set_text(&dateLayer, str_date);

		// Day of week
		text_layer_set_text(&DayOfWeekLayer,
				DAY_NAME_LANGUAGE[current_time->tm_wday]);

		the_last_hour = display_hour;
	}

}

void handle_minute_tick(AppContextRef ctx, PebbleTickEvent *t) {
	(void) ctx;

	update_display(t->tick_time);

	if (!located || !(t->tick_time->tm_min % 15)) {
		// Every 15 minutes, request updated weather
		http_location_request();
	}

	// Every 15 minutes, request updated time
	http_time_request();

	if (!calculated_sunset_sunrise) {
		// Start with some default values
		text_layer_set_text(&text_sunrise_layer, "Wait!");
		text_layer_set_text(&text_sunset_layer, "Wait!");
	}

	if (!(t->tick_time->tm_min % 2) || data.link_status == LinkStatusUnknown)
		link_monitor_ping();
}

void handle_init(AppContextRef ctx) {
	(void) ctx;

	window_init(&window, "91 Weather");
	window_stack_push(&window, true /* Animated */);
	window_set_background_color(&window, GColorWhite);
//	GColorWhite
	resource_init_current_app(&APP_RESOURCES);

	// Load Fonts
	font_temperature = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FUTURA_40));
	font_time= fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_ARIAL_30_BOLD));
	font_date = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_HELVETICA_18_BOLD));

	// Day of week text
	text_layer_init(&DayOfWeekLayer,
			GRect(5, 0, 130 /* width */, 30 /* height */));
	layer_add_child(&window.layer, &DayOfWeekLayer.layer);
	text_layer_set_text_color(&DayOfWeekLayer, GColorBlack);
	text_layer_set_background_color(&DayOfWeekLayer, GColorClear);
	text_layer_set_font(&DayOfWeekLayer, font_date);
	text_layer_set_text_alignment(&DayOfWeekLayer, GTextAlignmentCenter);

	// Date info text
	text_layer_init(&dateLayer, GRect(5, 25, 130 /* width */, 30 /* height */));
	layer_add_child(&window.layer, &dateLayer.layer);
	text_layer_set_text_color(&dateLayer, GColorBlack);
	text_layer_set_background_color(&dateLayer, GColorClear);
	text_layer_set_font(&dateLayer, font_date);
	text_layer_set_text_alignment(&dateLayer, GTextAlignmentCenter);


	// Clock Text
	text_layer_init(&timeLayer,
			GRect(0, 47, 150 /* width */, 50 /* height */));
	layer_add_child(&window.layer, &timeLayer.layer);
	text_layer_set_text_color(&timeLayer, GColorBlack);
	text_layer_set_background_color(&timeLayer, GColorClear);
	text_layer_set_font(&timeLayer, font_time);
	text_layer_set_text_alignment(&timeLayer, GTextAlignmentCenter);

	// Sunrise Text Title
	text_layer_init(&text_sunrise_title_layer, window.layer.frame);
	text_layer_set_text_color(&text_sunrise_title_layer, GColorBlack);
	text_layer_set_background_color(&text_sunrise_title_layer, GColorClear);
	layer_set_frame(&text_sunrise_title_layer.layer, GRect(15, 85, 100, 30));
	text_layer_set_font(&text_sunrise_title_layer,
			fonts_get_system_font(FONT_KEY_GOTHIC_14));
	text_layer_set_text(&text_sunrise_title_layer, "Sunrise");
	layer_add_child(&window.layer, &text_sunrise_title_layer.layer);

	// Sunrise Text
	text_layer_init(&text_sunrise_layer, window.layer.frame);
	text_layer_set_text_color(&text_sunrise_layer, GColorBlack);
	text_layer_set_background_color(&text_sunrise_layer, GColorClear);
	layer_set_frame(&text_sunrise_layer.layer, GRect(20, 100, 100, 30));
	text_layer_set_font(&text_sunrise_layer,
			fonts_get_system_font(FONT_KEY_GOTHIC_14));
	layer_add_child(&window.layer, &text_sunrise_layer.layer);

	// Sunset Text Title
	text_layer_init(&text_sunset_title_layer, window.layer.frame);
	text_layer_set_text_color(&text_sunset_title_layer, GColorBlack);
	text_layer_set_background_color(&text_sunset_title_layer, GColorClear);
	layer_set_frame(&text_sunset_title_layer.layer, GRect(80, 85, 100, 30));
	text_layer_set_font(&text_sunset_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
	text_layer_set_text(&text_sunset_title_layer, "Sunset");
	layer_add_child(&window.layer, &text_sunset_title_layer.layer);

	// Sunset Text
	text_layer_init(&text_sunset_layer, window.layer.frame);
	text_layer_set_text_color(&text_sunset_layer, GColorBlack);
	text_layer_set_background_color(&text_sunset_layer, GColorClear);
	layer_set_frame(&text_sunset_layer.layer, GRect(85, 100, 100, 30));
	text_layer_set_font(&text_sunset_layer,
			fonts_get_system_font(FONT_KEY_GOTHIC_14));
	layer_add_child(&window.layer, &text_sunset_layer.layer);

	// Text for Temperature
	text_layer_init(&text_temperature_layer, window.layer.frame);
	text_layer_set_text_color(&text_temperature_layer, GColorBlack);
	text_layer_set_background_color(&text_temperature_layer, GColorClear);
	layer_set_frame(&text_temperature_layer.layer, GRect(68, 120, 64, 68));
	text_layer_set_font(&text_temperature_layer, font_temperature);
	layer_add_child(&window.layer, &text_temperature_layer.layer);

//	// Calls Info layer
//	text_layer_init(&calls_layer, window.layer.frame);
//	text_layer_set_text_color(&calls_layer, GColorWhite);
//	text_layer_set_background_color(&calls_layer, GColorClear);
//	layer_set_frame(&calls_layer.layer, GRect(12, 135, 100, 30));
//	text_layer_set_font(&calls_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
//	layer_add_child(&window.layer, &calls_layer.layer);
//	text_layer_set_text(&calls_layer, "?");

//	// SMS Info layer
//	text_layer_init(&sms_layer, window.layer.frame);
//	text_layer_set_text_color(&sms_layer, GColorWhite);
//	text_layer_set_background_color(&sms_layer, GColorClear);
//	layer_set_frame(&sms_layer.layer, GRect(41, 135, 100, 30));
//	text_layer_set_font(&sms_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
//	layer_add_child(&window.layer, &sms_layer.layer);
//	text_layer_set_text(&sms_layer, "?");

	// DEBUG Info layer 
	text_layer_init(&debug_layer, window.layer.frame);
	text_layer_set_text_color(&debug_layer, GColorWhite);
	text_layer_set_background_color(&debug_layer, GColorClear);
	layer_set_frame(&debug_layer.layer, GRect(50, 152, 100, 30));
	text_layer_set_font(&debug_layer,
			fonts_get_system_font(FONT_KEY_GOTHIC_14));
	layer_add_child(&window.layer, &debug_layer.layer);

	data.link_status = LinkStatusUnknown;
	link_monitor_ping();

	// request data refresh on window appear (for example after notification)
	WindowHandlers handlers = { .appear = &link_monitor_ping };
	window_set_window_handlers(&window, handlers);

	http_register_callbacks(
			(HTTPCallbacks) {.failure=failed,.success=success,.reconnect=reconnect,.location=location,.time=receivedtime}, (void*)ctx);
	register_callbacks();

	// Avoids a blank screen on watch start.
	PblTm tick_time;

	get_time(&tick_time);
	update_display(&tick_time);

}

void handle_deinit(AppContextRef ctx) {
	(void) ctx;

	fonts_unload_custom_font(font_temperature);

}

void pbl_main(void *params) {
	PebbleAppHandlers handlers = { .init_handler = &handle_init,
			.deinit_handler = &handle_deinit, .tick_info = { .tick_handler =
					&handle_minute_tick, .tick_units = MINUTE_UNIT },
			.messaging_info = { .buffer_sizes = { .inbound = 124, .outbound =
					256, } } };
	app_event_loop(params, &handlers);
}

void request_weather() {

	if (!located) {
		http_location_request();
		return;
	}

	// Build the HTTP request
	DictionaryIterator *body;
	HTTPResult result = http_out_get("https://ofkorth.net/pebble/weather.php",
			WEATHER_HTTP_COOKIE, &body);
	if (result != HTTP_OK) {
		return;
	}

	dict_write_int32(body, WEATHER_KEY_LATITUDE, our_latitude);
	dict_write_int32(body, WEATHER_KEY_LONGITUDE, our_longitude);
	dict_write_cstring(body, WEATHER_KEY_UNIT_SYSTEM, UNIT_SYSTEM);

	// Send it.
	if (http_out_send() != HTTP_OK) {
		return;
	}

	// Request updated Time
	http_time_request();
}
