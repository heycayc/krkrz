/**
 * メモ
 *
 * JNIとのやり取りに関して、各種寿命など以下のリンク文書が参考になる
 * Android開発者のためのJNI入門
 * http://techbooster.jpn.org/andriod/application/7264/
 * Javaを呼出して動かす（jobject、jstring、jclass）
 * http://simple-asta.blogspot.jp/p/javajobjectjstringjclass.html
 */
#include "tjsCommHead.h"

#include "tjsError.h"
#include "tjsDebug.h"

#include <jni.h>
#include <errno.h>
#include <dirent.h>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <android/sensor.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/asset_manager_jni.h>
#include "Application.h"

#include "ScriptMgnIntf.h"
#include "SystemIntf.h"
#include "DebugIntf.h"
#include "TickCount.h"
#include "NativeEventQueue.h"
#include "CharacterSet.h"
#include "WindowForm.h"
#include "SysInitImpl.h"
#include "SystemControl.h"
#include "ActivityEvents.h"
#include "MsgIntf.h"
#include "FontSystem.h"
#include "GraphicsLoadThread.h"

#include <ft2build.h>
#include FT_TRUETYPE_UNPATENTED_H
#include FT_SYNTHESIS_H
#include FT_BITMAP_H
extern FT_Library FreeTypeLibrary;
extern void TVPInitializeFont();

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "krkrz", __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, "krkrz", __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, "krkrz", __VA_ARGS__))

static const tjs_int TVP_VERSION_MAJOR = 1;
static const tjs_int TVP_VERSION_MINOR = 0;
static const tjs_int TVP_VERSION_RELEASE = 0;
static const tjs_int TVP_VERSION_BUILD = 1;

tTVPApplication* Application;

extern void TVPRegisterAssetMedia();
extern void TVPRegisterContentMedia();
/**
 * Android 版のバージョン番号はソースコードに埋め込む
 * パッケージのバージョン番号はアプリのバージョンであって、エンジンのバージョンではないため
 * apk からバージョン番号を取得するのは好ましくない。
 */
void TVPGetFileVersionOf( tjs_int& major, tjs_int& minor, tjs_int& release, tjs_int& build ) {
	major = TVP_VERSION_MAJOR;
	minor = TVP_VERSION_MINOR;
	release = TVP_VERSION_RELEASE;
	build = TVP_VERSION_BUILD;
}

/**
 * WM_... のように AM_... でメッセージを作ると理解早いかな
 *
 * 初期化は複数段階で行う必要がある
 * native 初期化 > Java 側初期化 > Java側からnativeへ諸設定 > nativeスレッド本格始動
 */

tTVPApplication::tTVPApplication()
: jvm_(nullptr), window_(nullptr), asset_manager_(nullptr), config_(nullptr), is_terminate_(true), main_window_(nullptr),
 console_cache_(1024), image_load_thread_(nullptr), thread_id_(-1)
{
}
tTVPApplication::~tTVPApplication() {
}

NativeEvent* tTVPApplication::createNativeEvent() {
	std::lock_guard<std::mutex> lock( command_cache_mutex_ );
	if( command_cache_.empty() ) {
		return new NativeEvent();
	} else {
		NativeEvent* ret = command_cache_.back();
		command_cache_.pop_back();
		return ret;
	}
}
void tTVPApplication::releaseNativeEvent( NativeEvent* ev ) {
	std::lock_guard<std::mutex> lock( command_cache_mutex_ );
	command_cache_.push_back( ev );
}
// コマンドをメインのメッセージループに投げる
void tTVPApplication::postEvent( const NativeEvent* ev, NativeEventQueueIntarface* handler ) {
	NativeEvent* e = createNativeEvent();
	e->Message = ev->Message;
	e->WParam = ev->WParam;
	e->LParam = ev->LParam;
	{
		std::lock_guard<std::mutex> lock( command_que_mutex_ );
		command_que_.push( EventCommand( handler, e ) );
	}

	// メインスレッドを起こす
	wakeupMainThread();
}
void tTVPApplication::wakeupMainThread() {
	main_thread_cv_.notify_one();
}
void tTVPApplication::mainLoop() {
	bool attached;
	JNIEnv *env = getJavaEnv(attached);	// attach thread to java
	// ここの env は、TJS VM のメインスレッド内共通なので、スレッドIDと共に保持して、各種呼び出し時に使いまわす方が効率的か
	while( is_terminate_ == false ) {
		{	// イベントキューからすべてのイベントをディスパッチ
			std::lock_guard<std::mutex> lock( command_que_mutex_ );
			while( !command_que_.empty() ) {
				NativeEventQueueIntarface* handler = command_que_.front().target;
				NativeEvent* event = command_que_.front().command;
				command_que_.pop();

				if( handler != nullptr ) {
					// ハンドラ指定付きの場合はハンドラから探して見つからったらディスパッチ
					std::lock_guard<std::mutex> lock( event_handlers_mutex_ );
					auto result = std::find_if(event_handlers_.begin(), event_handlers_.end(), [handler](NativeEventQueueIntarface* x) { return x == handler; });
					if( result != event_handlers_.end() ) {
						(*result)->Dispatch( *event );
					}
				} else {
					if( appDispatch(*event) == false ) {
						// ハンドラ指定のない場合でアプリでディスパッチしないものは、すべてのハンドラでディスパッチ
						std::lock_guard<std::mutex> lock( event_handlers_mutex_ );
						for (std::vector<NativeEventQueueIntarface *>::iterator it = event_handlers_.begin();
							 it != event_handlers_.end(); it++) {
							if ((*it) != nullptr) {
								(*it)->Dispatch(*event);
							}
						}
					}
				}
				releaseNativeEvent( event );
			}
			// アイドル処理
			handleIdle();
		}
		{	// コマンドキューに何か入れられるまで待つ
			std::unique_lock<std::mutex> uniq_lk(command_que_mutex_);
			main_thread_cv_.wait(uniq_lk, [this]{ return !command_que_.empty();});
		}
	}
	if( attached ) detachJavaEnv();
}
bool tTVPApplication::appDispatch(NativeEvent& ev) {
	switch( ev.Message ) {
		case AM_STARTUP_SCRIPT:
			TVPInitializeStartupScript();
			return true;
	}
	return false;
}
/*
int8_t tTVPApplication::readCommand() {
	int8_t cmd;
	if( read(user_msg_read_, &cmd, sizeof(cmd)) == sizeof(cmd) ) {
		return cmd;
	} else {
		LOGE("No data on command pipe!");
	}
	return -1;
}
int tTVPApplication::messagePipeCallBack(int fd, int events, void* user) {
	if( user != NULL ) {
		tTVPApplication* app = (tTVPApplication*)user;
		NativeEvent msg;
		while( read(fd, &msg, sizeof(NativeEvent)) == sizeof(NativeEvent) ) {
			app->HandleMessage(msg);
		}
	}
	return 1;
}
 */
void tTVPApplication::HandleMessage( NativeEvent& ev ) {
	std::lock_guard<std::mutex> lock( event_handlers_mutex_ );
	for( std::vector<NativeEventQueueIntarface*>::iterator it = event_handlers_.begin(); it != event_handlers_.end(); it++ ) {
		if( (*it) != NULL ) (*it)->Dispatch( ev );
	}
}
// for iTVPApplication
void tTVPApplication::startApplication( struct android_app* state ) {
	/*
	assert( state );
	app_state_ = state;

	state->userData = this;
	state->onAppCmd = tTVPApplication::handleCommand;
	state->onInputEvent = tTVPApplication::handleInput;
	initCommandPipe();

	if( state->savedState != NULL ) {
		// We are starting with a previous saved state; restore from it.
		loadSaveState( state->savedState );
	}
	*/

	//print_font_files();
	// ここから初期化
	
	// try starting the program!
	bool engine_init = false;
	try {
		// TJS2 スクリプトエンジンを初期化してstartup.tjsを呼ぶ。
		TVPInitScriptEngine();
		engine_init = true;

		// banner
		TVPAddImportantLog( TVPFormatMessage(TVPProgramStartedOn, TVPGetOSName(), TVPGetPlatformName()) );
		
		// main loop
		tjs_uint32 tick = TVPGetRoughTickCount32();
		while( 1 ) { // Read all pending events.
			int ident;
			int events;
			//struct android_poll_source* source;
			void* source;
			int timeout = 16;	//16msec周期で動作するようにする
			while( (ident = ALooper_pollAll( timeout/* msec */, NULL, &events, (void**)&source)) != ALOOPER_POLL_TIMEOUT ) {
				// Process this event.
				if( source != NULL ) {
					if( (tTVPApplication*)source == this ) {
						// user_msg_write_ へ投げられたコマンド
					} else {
						struct android_poll_source* ps = (struct android_poll_source*)source;
						ps->process(state, ps);
					}
				}

				// If a sensor has data, process it now.
				/*
				if( ident == LOOPER_ID_USER ) {
					handleSensorEvent();
				}
				*/

				// Check if we are exiting.
				if( state->destroyRequested != 0 ) {
					tarminateProcess();
					return;
				}
				tjs_uint32 curtick = TVPGetRoughTickCount32();
				if( tick > curtick ) {	// 1周回ってしまった場合
					curtick += 0xffffffffUL - tick;
					tick = 0;
				}
				timeout = 16 - (curtick - tick);
				if( timeout < 0 ) timeout = 0;
			}
			handleIdle();
			tick = TVPGetRoughTickCount32();
		}
	} catch(...) {
	}
}
void tTVPApplication::initializeApplication() {
	TVPTerminateCode = 0;

	try {
		// asset:// を登録
		TVPRegisterAssetMedia();

		// content:// を登録
		TVPRegisterContentMedia();

		// スクリプトエンジンを初期化し各種クラスを登録
		TVPInitScriptEngine();

		// ログへOS名等出力
		TVPAddImportantLog( TVPFormatMessage(TVPProgramStartedOn, TVPGetOSName(), TVPGetPlatformName()) );

		// アーカイブデリミタ、カレントディレクトリ、msgmap.tjsの実行 と言った初期化処理
		TVPInitializeBaseSystems();

		// -userconf 付きで起動されたかどうかチェックする。Android だと Activity 分けた方が賢明
		// if(TVPExecuteUserConfig()) return;

		// TODO 非同期画像読み込みは後で実装する
		image_load_thread_ = new tTVPAsyncImageLoader();

		TVPSystemInit();

		SetTitle( tjs_string(TVPKirikiri) );

		TVPSystemControl = new tTVPSystemControl();

#ifndef TVP_IGNORE_LOAD_TPM_PLUGIN
//		TVPLoadPluigins(); // load plugin module *.tpm
#endif

		// start image load thread
		image_load_thread_->StartTread();

		if(TVPProjectDirSelected) TVPInitializeStartupScript();

		// run main loop from activity resume.
	} catch(...) {
	}
}
void tTVPApplication::handleCommand( struct android_app* state, int32_t cmd ) {
	tTVPApplication* app = (tTVPApplication*)(state->userData);
	app->onCommand( state, cmd );
}
int32_t tTVPApplication::handleInput( struct android_app* state, AInputEvent* event ) {
	tTVPApplication* app = (tTVPApplication*)(state->userData);
	return app->onInput( state, event );
}
void* tTVPApplication::startMainLoopCallback( void* myself ) {
	tTVPApplication* app = reinterpret_cast<tTVPApplication*>(myself);
	app->mainLoop();
	pthread_exit(0);
	return nullptr;
}
void tTVPApplication::startMainLoop() {
	if( is_terminate_ ) {
		is_terminate_ = false;
		pthread_attr_t attr;
		if( pthread_attr_init( &attr ) == 0 ) {
			pthread_attr_setstacksize( &attr, 64*1024 );
			pthread_create( &thread_id_, &attr, startMainLoopCallback, this );
			pthread_attr_destroy( &attr );
		} else {
			pthread_create( &thread_id_, 0, startMainLoopCallback, this );
		}
	}
}
void tTVPApplication::stopMainLoop() {
	if( is_terminate_ == false ) {
		is_terminate_ = true;
		wakeupMainThread();
		pthread_join( thread_id_, 0 );
	}
}
void tTVPApplication::onCommand( struct android_app* state, int32_t cmd ) {
	switch( cmd ) {
		case APP_CMD_SAVE_STATE:
			saveState();
			break;
		case APP_CMD_INIT_WINDOW:
			initializeWindow();
			break;
		case APP_CMD_TERM_WINDOW:
			tarminateWindow();
			break;
        case APP_CMD_GAINED_FOCUS:
			gainedFocus();
			break;
		case APP_CMD_LOST_FOCUS:
			lostFocus();
			break;
		case APP_CMD_INPUT_CHANGED:
			inputChanged();
			break;
		case APP_CMD_WINDOW_RESIZED:
			windowResized();
			break;
		case APP_CMD_WINDOW_REDRAW_NEEDED:
			windowRedrawNeeded();
			break;
		case APP_CMD_CONTENT_RECT_CHANGED:
			contentRectChanged();
			break;
		case APP_CMD_CONFIG_CHANGED:
			configChanged();
			break;
		case APP_CMD_LOW_MEMORY:
			lowMemory();
			break;
		case APP_CMD_START:
			onStart();
			break;
		case APP_CMD_RESUME:
			onResume();
			break;
		case APP_CMD_PAUSE:
			onPause();
			break;
		case APP_CMD_STOP:
			onStop();
			break;
		case APP_CMD_DESTROY:
			onDestroy();
			break;
	}
}
int32_t tTVPApplication::onInput( struct android_app* state, AInputEvent* event ) {
	int32_t type = AInputEvent_getType(event);
	if( type == AINPUT_EVENT_TYPE_MOTION ) {
		int32_t src = AInputEvent_getSource(event);	// 入力デバイスの種類
		// src == AINPUT_SOURCE_TOUCHSCREEN タッチスクリーン
		// src == AINPUT_SOURCE_MOUSE AINPUT_SOURCE_TRACKBALL AINPUT_SOURCE_TOUCHPAD
		int32_t action = AMotionEvent_getAction(event);
		int32_t meta = AMotionEvent_getMetaState(event);
		// AMotionEvent_getEventTime(event); // イベント発生時間
		// AMotionEvent_getDownTime(event); // 押されていた時間
		// AMotionEvent_getEdgeFlags(event); // スクリーン端判定
		float x = AMotionEvent_getX(event, 0);
		float y = AMotionEvent_getY(event, 0);
		float cy = AMotionEvent_getTouchMajor(event,0);	// 触れられている長辺 指の形状から縦側にしておく
		float cx = AMotionEvent_getTouchMinor(event,0);	// 触れられている短辺 指の形状から横側にしておく
		float pressure = AMotionEvent_getPressure(event, 0);	// 圧力
		/*
		float size = AMotionEvent_getSize(event, 0);	// 範囲(推定値) デバイス固有値から0-1の範囲に正規化したもの
		float toolmajor = AMotionEvent_getToolMajor(event,0);
		float toolminor = AMotionEvent_getToolMinor(event,0);
		LOGI( "press : %f, size: %f, major : %f, minor : %f\n", pressure, size, toolmajor, toolminor );
		*/
		int32_t id = AMotionEvent_getPointerId(event, 0);
		action &= AMOTION_EVENT_ACTION_MASK;
		switch( action ) {
		case AMOTION_EVENT_ACTION_DOWN:
			OnTouchDown( x, y, cx, cy, id, pressure, meta );
			break;
		case AMOTION_EVENT_ACTION_UP:
			OnTouchUp( x, y, cx, cy, id, pressure, meta );
			break;
		case AMOTION_EVENT_ACTION_CANCEL:	// Down/Up同時発生。ありえるの？
			break;
		case AMOTION_EVENT_ACTION_MOVE:
			OnTouchMove( x, y, cx, cy, id, pressure, meta );
			break;
		case AMOTION_EVENT_ACTION_POINTER_DOWN: {	// multi-touch
			size_t downidx = (action&AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
			size_t count = AMotionEvent_getPointerCount(event);
			if( downidx == 0 ) {
				OnTouchDown( x, y, cx, cy, id, pressure, meta );
			} else {
				OnTouchMove( x, y, cx, cy, id, pressure, meta );
			}
			for( size_t i = 1; i < count; i++ ) {
				x = AMotionEvent_getX(event, i);
				y = AMotionEvent_getY(event, i);
				cy = AMotionEvent_getTouchMajor(event,i);
				cx = AMotionEvent_getTouchMinor(event,i);
				pressure = AMotionEvent_getPressure(event, i);
				id = AMotionEvent_getPointerId(event, i);
				if( i == downidx ) {
					OnTouchDown( x, y, cx, cy, id, pressure, meta );
				} else {
					OnTouchMove( x, y, cx, cy, id, pressure, meta );
				}
			}
			break;
		}
		case AMOTION_EVENT_ACTION_POINTER_UP: {	// multi-touch
			size_t upidx = (action&AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
			size_t count = AMotionEvent_getPointerCount(event);
			if( upidx == 0 ) {
				OnTouchUp( x, y, cx, cy, id, pressure, meta );
			} else {
				OnTouchMove( x, y, cx, cy, id, pressure, meta );
			}
			for( size_t i = 1; i < count; i++ ) {
				x = AMotionEvent_getX(event, i);
				y = AMotionEvent_getY(event, i);
				cy = AMotionEvent_getTouchMajor(event,i);
				cx = AMotionEvent_getTouchMinor(event,i);
				pressure = AMotionEvent_getPressure(event, i);
				id = AMotionEvent_getPointerId(event, i);
				if( i == upidx ) {
					OnTouchUp( x, y, cx, cy, id, pressure, meta );
				} else {
					OnTouchMove( x, y, cx, cy, id, pressure, meta );
				}
			}
			break;
		}
		case AMOTION_EVENT_ACTION_OUTSIDE:
			break;
		}
		return 1;
	} else if( type == AINPUT_EVENT_TYPE_KEY ) { // key events
		int32_t src = AInputEvent_getSource(event);	// 入力デバイスの種類
		// src == AINPUT_SOURCE_KEYBOARD AINPUT_SOURCE_DPAD
		return 1;
	}
	return 0;
}

#if 0
void tTVPApplication::setApplicationState( struct android_app* state ) {
	assert( state );
	app_state_ = state;
	// ここでいろいろと初期化してしまった方がよさげ

	std::string internalDataPath( getInternalDataPath() );
	TVPUtf8ToUtf16( internal_data_path_, internalDataPath );

	std::string externalDataPath( getExternalDataPath() );
	TVPUtf8ToUtf16( external_data_path_, externalDataPath );

	/*
	// Prepare to monitor accelerometer
	sensor_manager_ = ASensorManager_getInstance();
	accelerometer_sensor_ = ASensorManager_getDefaultSensor( sensorManager, ASENSOR_TYPE_ACCELEROMETER );
	sensor_event_queue_ = ASensorManager_createEventQueue( sensorManager, state->looper, LOOPER_ID_USER, NULL, NULL );
	*/
}
#endif
void tTVPApplication::loadSaveState( void* state ) {
}
void tTVPApplication::handleSensorEvent() {
}
void tTVPApplication::tarminateProcess() {
	//screen_.tarminate();
}
void tTVPApplication::handleIdle() {
}
void tTVPApplication::saveState() {
	//clearSaveState();
}
void tTVPApplication::initializeWindow() {
	//screen_.initialize(this);
}
void tTVPApplication::tarminateWindow() {
	//screen_.tarminate();
}
void tTVPApplication::gainedFocus() {
}
void tTVPApplication::lostFocus() {
}
void tTVPApplication::inputChanged() {
}
void tTVPApplication::windowResized() {
}
void tTVPApplication::windowRedrawNeeded() {
}
void tTVPApplication::contentRectChanged() {
}
void tTVPApplication::configChanged() {
}
void tTVPApplication::lowMemory() {
}
void tTVPApplication::onStart() {
}
void tTVPApplication::onResume() {
}
void tTVPApplication::onPause() {
}
void tTVPApplication::onStop() {
}
void tTVPApplication::onDestroy() {
}
void tTVPApplication::OnTouchDown( float x, float y, float cx, float cy, int32_t id, float pressure, int32_t meta ) {
	//screen_.OnTouchDown( x, y, cx, cy, id );
}
void tTVPApplication::OnTouchMove( float x, float y, float cx, float cy, int32_t id, float pressure,int32_t meta ) {
	//screen_.OnTouchMove( x, y, cx, cy, id );
}
void tTVPApplication::OnTouchUp( float x, float y, float cx, float cy, int32_t id, float pressure,int32_t meta ) {
	//screen_.OnTouchUp( x, y, cx, cy, id );
}
//-----------------------------

std::vector<std::string>* LoadLinesFromFile( const tjs_string& path ) {
	std::string npath;
	if( TVPUtf16ToUtf8( npath, path ) == false ) {
		return nullptr;
	}
	FILE *fp = fopen( npath.c_str(), "r");
    if( fp == nullptr ) {
		return nullptr;
    }
	char buff[1024];
	std::vector<std::string>* ret = new std::vector<std::string>();
    while( fgets(buff, 1024, fp) != nullptr ) {
		ret->push_back( std::string(buff) );
    }
    fclose(fp);
	return ret;
}
void tTVPApplication::writeBitmapToNative( const void * src ) {
	int32_t format = ANativeWindow_getFormat( window_ );
	ARect dirty;
	dirty.left = 0;
	dirty.top = 0;
	dirty.right = ANativeWindow_getWidth( window_ );
	dirty.bottom = ANativeWindow_getHeight( window_ );
	ANativeWindow_Buffer buffer;
	ANativeWindow_lock( window_ , &buffer, &dirty );
	unsigned char* bits = (unsigned char*)buffer.bits;
	for( int32_t y = 0; y < buffer.height; y++ ) {
		unsigned char* lines = bits;
		for( int32_t x = 0; x < buffer.width; x++ ) {
			// src を書き込む
			lines[0] = 0xff;
			lines[1] = 0xff;
			lines[2] = 0;
			lines[3] = 0xff;
			lines += 4;
		}
		bits += buffer.stride*sizeof(int32_t);
	}
	ANativeWindow_unlockAndPost( window_  );
}
void tTVPApplication::AddWindow( TTVPWindowForm* window ) {
	if( main_window_ ) {
		TVPThrowExceptionMessage(TJS_W("Cannot add window."));	// TODO move to resource
	}
	main_window_ = window;
}
void tTVPApplication::PrintConsole( const tjs_char* mes, unsigned long len, bool iserror ) {
	if( console_cache_.size() < (len*3+1) ) {
		console_cache_.resize(len*3+1);
	}
	tjs_int u8len = TVPWideCharToUtf8String( mes, &(console_cache_[0]) );
	console_cache_[u8len] = '\0';
	if( iserror ) {
		__android_log_print(ANDROID_LOG_ERROR, "krkrz", "%s", &(console_cache_[0]) );
	} else {
		__android_log_print(ANDROID_LOG_INFO, "krkrz", "%s", &(console_cache_[0]) );
	}
}
extern "C" {
iTVPApplication* CreateApplication() {
	Application = new tTVPApplication();
	return Application;
}
void DestroyApplication( iTVPApplication* app ) {
	delete app;
	Application = NULL;
}
};

// /system/fonts/ フォントが置かれているフォルダから取得する(Nexus5で約50msかかる)
// フォントが最初に使われる時にFontSystem::InitFontNames経由で呼ばれる
extern void TVPAddSystemFontToFreeType( const std::string& storage, std::vector<tjs_string>* faces );
void TVPGetAllFontList( std::vector<tjs_string>& list ) {
	TVPInitializeFont();

	DIR* dr;
	if( ( dr = opendir("/system/fonts/") ) != nullptr ) {
		struct dirent* entry;
		while( ( entry = readdir( dr ) ) != nullptr ) {
			if( entry->d_type == DT_REG ) {
				std::string path(entry->d_name);
				std::string::size_type extp = path.find_last_of(".");
				if( extp != std::string::npos ) {
					std::string ext = path.substr(extp);
					if( ext == std::string(".ttf") || ext == std::string(".ttc") || ext == std::string(".otf") ) {
						// .ttf | .ttc | .otf
						std::string fullpath( std::string("/system/fonts/") + path );
						TVPAddSystemFontToFreeType( fullpath, &list );
					}
				}
			}
		}
		closedir( dr );
	}
#if 0
	for( std::list<std::string>::const_iterator i = fontfiles.begin(); i != fontfiles.end(); ++i ) {
		FT_Face face = nullptr;
		std::string fullpath( std::string("/system/fonts/") + *i );
		FT_Open_Args args;
		memset(&args, 0, sizeof(args));
		args.flags = FT_OPEN_PATHNAME;
		args.pathname = fullpath.c_str();
		tjs_uint face_num = 1;
		std::list<std::string> facenames;
		for( tjs_uint f = 0; f < face_num; f++ ) {
			FT_Error err = FT_Open_Face( FreeTypeLibrary, &args, 0, &face);
			if( err == 0 ) {
				facenames.push_back( std::string(face->family_name) );
				std::string(face->style_name);	// スタイル名
				if( face->face_flags & FT_FACE_FLAG_SCALABLE ) {
					// 可変サイズフォントのみ採用
					if( face->num_glyphs > 2965 ) {
						// JIS第一水準漢字以上のグリフ数
						if( face->style_flags & FT_STYLE_FLAG_ITALIC ) {}
						if( face->style_flags & FT_STYLE_FLAG_BOLD ) {}
						face_num = face->num_faces;
						int numcharmap = face->num_charmaps;
						for( int c = 0; c < numcharmap; c++ ) {
							FT_Encoding enc = face->charmaps[c]->encoding;
							if( enc == FT_ENCODING_SJIS ) {
								// mybe japanese
							}
							if( enc == FT_ENCODING_UNICODE ) {
							}
						}
					}
				}
			}
			if(face) FT_Done_Face(face), face = nullptr;
		}
	}
#endif
}
static bool IsInitDefalutFontName = false;
//extern FontSystem* TVPFontSystem;
static bool SelectFont( const std::vector<tjs_string>& faces, tjs_string& face ) {
	std::vector<tjs_string> fonts;
	TVPGetAllFontList( fonts );
	for( auto i = faces.begin(); i != faces.end(); ++i ) {
		auto found = std::find( fonts.begin(), fonts.end(), *i );
		//if( TVPFontSystem->FontExists( *i ) ) {
		if( found != fonts.end() ) {
			face = *i;
			return true;
		}
	}
	return false;
}
const tjs_char *TVPGetDefaultFontName() {
	if( IsInitDefalutFontName ) {
		return TVPDefaultFontName;
	}
	TVPDefaultFontName.AssignMessage(TJS_W("Droid Sans Mono"));
	IsInitDefalutFontName =  true;

	// コマンドラインで指定がある場合、そのフォントを使用する
	tTJSVariant opt;
	if(TVPGetCommandLine(TJS_W("-deffont"), &opt)) {
		ttstr str(opt);
		TVPDefaultFontName.AssignMessage( str.c_str() );
	} else {
		std::string lang( Application->getLanguage() );
		tjs_string face;
		if( lang == std::string("ja" ) ) {
			std::vector<tjs_string> facenames{tjs_string(TJS_W("Noto Sans JP")),tjs_string(TJS_W("MotoyaLMaru")),
				tjs_string(TJS_W("MotoyaLCedar")),tjs_string(TJS_W("Droid Sans Japanese")),tjs_string(TJS_W("Droid Sans Mono"))};
			if( SelectFont( facenames, face ) ) {
				TVPDefaultFontName.AssignMessage( face.c_str() );
			}
		} else if( lang == std::string("zh" ) ) {
			std::vector<tjs_string> facenames{tjs_string(TJS_W("Noto Sans SC")),tjs_string(TJS_W("Droid Sans Mono"))};
			if( SelectFont( facenames, face ) ) {
				TVPDefaultFontName.AssignMessage( face.c_str() );
			}
		} else if( lang == std::string("ko" ) ) {
			std::vector<tjs_string> facenames{tjs_string(TJS_W("Noto Sans KR")),tjs_string(TJS_W("Droid Sans Mono"))};
			if( SelectFont( facenames, face ) ) {
				TVPDefaultFontName.AssignMessage( face.c_str() );
			}
		} else {
			std::vector<tjs_string> facenames{tjs_string(TJS_W("Droid Sans Mono"))};
			if( SelectFont( facenames, face ) ) {
				TVPDefaultFontName.AssignMessage( face.c_str() );
			}
		}
	}
	return TVPDefaultFontName;
}
void TVPSetDefaultFontName( const tjs_char * name ) {
	TVPDefaultFontName.AssignMessage( name );
}

void tTVPApplication::getStringFromJava( const char* methodName, tjs_string& dest ) const {
	bool attached;
	JNIEnv *env = getJavaEnv(attached);
	if ( env != nullptr ) {
		jobject thiz = activity_;
		jclass clazz = env->GetObjectClass(thiz);
		jmethodID mid = env->GetMethodID(clazz, methodName, "()Ljava/lang/String;");
		jstring ret = (jstring) env->CallObjectMethod(thiz, mid, nullptr);
		int jstrlen = env->GetStringLength(ret);
		const jchar* chars = env->GetStringChars( ret, nullptr );
		dest = tjs_string( chars, &chars[jstrlen] );
		env->ReleaseStringChars( ret, chars );
		env->DeleteLocalRef( ret );
		env->DeleteLocalRef(clazz);
		if( attached ) detachJavaEnv();
	}
}
void tTVPApplication::setStringToJava( const char* methodName, const tjs_string& src ) {
	bool attached;
	JNIEnv *env = getJavaEnv(attached);
	if ( env != nullptr ) {
		jobject thiz = activity_;
		jclass clazz = env->GetObjectClass(thiz);
		jmethodID mid = env->GetMethodID(clazz, methodName, "(Ljava/lang/String;)V");
		jstring arg = env->NewString( reinterpret_cast<const jchar *>(src.c_str()), src.length() );
		env->CallVoidMethod(thiz, mid, arg);
		env->DeleteLocalRef( arg );
		env->DeleteLocalRef(clazz);
		if( attached ) detachJavaEnv();
	}
}
void tTVPApplication::callActivityMethod( const char* methodName ) const {
	bool attached;
	JNIEnv *env = getJavaEnv(attached);
	if ( env != nullptr ) {
		jobject thiz = activity_;
		jclass clazz = env->GetObjectClass(thiz);
		jmethodID mid = env->GetMethodID(clazz, methodName, "()V");
		env->CallVoidMethod(thiz, mid, nullptr);
		env->DeleteLocalRef(clazz);
		if( attached ) detachJavaEnv();
	}
}
const tjs_string& tTVPApplication::GetInternalDataPath() const {
	if( internal_data_path_.empty() ) {
		getStringFromJava( static_cast<const char*>("getInternalDataPath"), const_cast<tjs_string&>(internal_data_path_) );
	}
	return internal_data_path_;
}
const tjs_string& tTVPApplication::GetExternalDataPath() const {
	if( external_data_path_.empty() ) {
		getStringFromJava( static_cast<const char*>("getExternalDataPath"), const_cast<tjs_string&>(external_data_path_) );
	}
	return external_data_path_;
}
const tjs_string* tTVPApplication::GetCachePath() const {
	if( cache_path_.empty() ) {
		getStringFromJava( static_cast<const char*>("getCachePath"), const_cast<tjs_string&>(cache_path_) );
	}
	return &cache_path_;
}
const tjs_char* tTVPApplication::GetPackageName() const {
	if( package_path_.empty() ) {
		getStringFromJava( static_cast<const char*>("getPackageName"), const_cast<tjs_string&>(package_path_) );
	}
	return package_path_.c_str();
}
const tjs_char* tTVPApplication::GetPackageCodePath() const {
	if( package_code_path_.empty() ) {
		getStringFromJava( static_cast<const char*>("getPackageCodePath"), const_cast<tjs_string&>(package_code_path_) );
	}
	return package_code_path_.c_str();
}
void tTVPApplication::finishActivity() {
	callActivityMethod( "postFinish" );
	stopMainLoop();
}
void tTVPApplication::changeSurfaceSize( int w, int h ) {
	bool attached;
	JNIEnv *env = getJavaEnv(attached);
	if ( env != nullptr ) {
		jobject thiz = activity_;
		jclass clazz = env->GetObjectClass(thiz);
		jmethodID mid = env->GetMethodID(clazz, "postChangeSurfaceSize", "(II)V");
		env->CallVoidMethod(thiz, mid, w, h );
		env->DeleteLocalRef(clazz);
		if( attached ) detachJavaEnv();
	}
}
const tjs_string& tTVPApplication::getSystemVersion() const {
	if( system_release_version_.empty() ) {
		bool attached;
		JNIEnv *env = getJavaEnv(attached);
		if ( env != nullptr ) {
			jclass versionClass = env->FindClass("android/os/Build$VERSION" );
			jfieldID releaseFieldID = env->GetStaticFieldID(versionClass, "RELEASE", "Ljava/lang/String;" );
			jstring ret = (jstring)env->GetStaticObjectField(versionClass, releaseFieldID );
			int jstrlen = env->GetStringLength(ret);
			const jchar* chars = env->GetStringChars( ret, nullptr );
			tjs_string& dest = const_cast<tjs_string&>(system_release_version_);
			dest = tjs_string( chars, &chars[jstrlen] );
			env->ReleaseStringChars( ret, chars );
			env->DeleteLocalRef( ret );
			env->DeleteLocalRef(versionClass);
			if( attached ) detachJavaEnv();
		}
	}
	return system_release_version_;
}
tjs_string tTVPApplication::GetActivityCaption() {
	tjs_string caption;
	getStringFromJava( "getCaption", caption );
	return caption;
}
void tTVPApplication::SetActivityCaption( const tjs_string& caption ) {
	setStringToJava( "postChangeCaption", caption );
}
void tTVPApplication::ShowToast( const tjs_char* text ) {
	setStringToJava( "postShowToastMessage", tjs_string(text) );
}
int tTVPApplication::MessageDlg( const tjs_string& string, const tjs_string& caption, int type, int button ) {
	Application->ShowToast( string.c_str() );
	return 0;
}

/**
 * Java から送られてきた各種イベントをここで処理する
 * イベントの種類に応じてアプリケーションとして処理するか、Windowに処理させるか判断
 */
void tTVPApplication::SendMessageFromJava( tjs_int message, tjs_int64 wparam, tjs_int64 lparam ) {
	// Main Windowが存在する場合はそのWindowへ送る
	// TODO startup.tjsがまだ呼ばれていない(ストレージ選択されていない)時に、イベントをキューに溜めるのはよろしくない。メインスレッド起動時にキューを空にするのが良いか？
	NativeEvent ev(message,lparam,wparam);
	switch( message ) {
	case AM_STARTUP_SCRIPT:
		postEvent( &ev, nullptr );
		return;
	case AM_START:
	case AM_RESTART:
		return;
	case AM_RESUME:
		if( TVPProjectDirSelected ) {
			startMainLoop();
		}
		break;
	case AM_PAUSE:
		break;
	case AM_STOP:
		break;
	case AM_DESTROY:
		stopMainLoop();
		return;
	case AM_SURFACE_CHANGED:
	case AM_SURFACE_CREATED:
	case AM_SURFACE_DESTORYED:
		break;
	}
	TTVPWindowForm* win = GetMainWindow();
	if( win ) {
		postEvent( &ev, win->GetEventHandler() );
	} else {
		postEvent( &ev, nullptr );
	}
}
static const int TOUCH_DOWN = 0;
static const int TOUCH_MOVE = 1;
static const int TOUCH_UP = 2;
void tTVPApplication::SendTouchMessageFromJava( tjs_int type, float x, float y, float c, int id, tjs_int64 tick ) {
	NativeEvent ev;
	switch( type ) {
	case TOUCH_DOWN:
		ev.Message = AM_TOUCH_DOWN;
		break;
	case TOUCH_MOVE:
		ev.Message = AM_TOUCH_MOVE;
		break;
	case TOUCH_UP:
		ev.Message = AM_TOUCH_UP;
		break;
	default:
		return;
	}
	ev.WParamf0 = x;
	ev.WParamf1 = y;
	ev.LParamf0 = c;
	ev.LParam1 = id;
	ev.Result = tick;
	TTVPWindowForm* win = GetMainWindow();
	if( win ) {
		postEvent( &ev, win->GetEventHandler() );
	} else {
		postEvent( &ev, nullptr );
	}
}
void tTVPApplication::setWindow( ANativeWindow* window ) {
	// std::lock_guard<std::mutex> lock( main_thread_mutex_ );
	if( window_ ) {
		// release previous window reference
		ANativeWindow_release( window_ );
	}

	window_ = window;

	if( window ) {
		// acquire window reference
		ANativeWindow_acquire( window );
	}
}
void tTVPApplication::nativeSetSurface(JNIEnv *jenv, jobject obj, jobject surface) {
	if( surface != 0 ) {
		ANativeWindow* window = ANativeWindow_fromSurface(jenv, surface);
		LOGI("Got window %p", window);
		Application->setWindow(window);
		Application->SendMessageFromJava( AM_SURFACE_CHANGED, 0, 0 );
	} else {
		LOGI("Releasing window");
		Application->setWindow(nullptr);
		Application->SendMessageFromJava( AM_SURFACE_DESTORYED, 0, 0 );
	}
	return;
}
void jstrcpy_maxlen(tjs_char *d, const jchar *s, size_t len)
{
	tjs_char ch;
	len++;
	while((ch=*s)!=0 && --len) *(d++) = ch, s++;
	*d = 0;
}
void tTVPApplication::nativeSetMessageResource(JNIEnv *jenv, jobject obj, jobjectArray mesarray) {
	int stringCount = jenv->GetArrayLength(mesarray);
	for( int i = 0; i < stringCount; i++ ) {
		jstring string = (jstring) jenv->GetObjectArrayElement( mesarray, i);
		int jstrlen = jenv->GetStringLength( string );
		const jchar* chars = jenv->GetStringChars( string, nullptr );
		// copy message. 解放しない
		tjs_char* mesres = new tjs_char[jstrlen+1];
		jstrcpy_maxlen( mesres, chars, jstrlen );
		mesres[jstrlen] = TJS_W('\0');
		jenv->ReleaseStringChars( string, chars );
		jenv->DeleteLocalRef( string );
	}
}

void tTVPApplication::nativeSetAssetManager(JNIEnv *jenv, jobject obj, jobject assetManager ) {
	AAssetManager* am = AAssetManager_fromJava( jenv, assetManager );
	Application->setAssetManager( am );
}
/**
 * Java からの通知はここに来る
 */
void tTVPApplication::nativeToMessage(JNIEnv *jenv, jobject obj, jint mes, jlong wparam, jlong lparam ) {
	Application->SendMessageFromJava( mes, lparam, wparam );
}
void tTVPApplication::nativeSetActivity(JNIEnv *env, jobject obj, jobject activity) {
	if( activity != nullptr ) {
		jobject globalactivity = env->NewGlobalRef(activity);
		Application->activity_ = globalactivity;
		// Activity の jclass や 必要となるメソッドIDはここで一気に取得してしまっていた方がいいかもしれない
		// jclass は頻繁に必要になるのでここで、メソッドIDは必要になった初回に取得が妥当か
	} else {
		if( Application->activity_ != nullptr ) {
			env->DeleteGlobalRef( Application->activity_ );
			Application->activity_ = nullptr;
		}
	}
}
void tTVPApplication::nativeInitialize(JNIEnv *jenv, jobject obj) {
	Application->initializeApplication();
}
void tTVPApplication::nativeOnTouch( JNIEnv *jenv, jobject obj, jint type, jfloat x, jfloat y, jfloat c, jint id, jlong tick ) {
	Application->SendTouchMessageFromJava( type, x, y, c, id, tick );
}
// 起動パスを渡された時呼び出される
extern void TVPSetProjectPath( const ttstr& path );
void tTVPApplication::nativeSetStartupPath( JNIEnv *jenv, jobject obj, jstring jpath ) {
	int jstrlen = jenv->GetStringLength( jpath );
	const jchar* chars = jenv->GetStringChars( jpath, nullptr );
	ttstr path(reinterpret_cast<const tjs_char*>(chars),jstrlen);
	//Application->setStartupPath( path );
	jenv->ReleaseStringChars( jpath, chars );

	TVPSetProjectPath( path );
	Application->SendMessageFromJava( AM_STARTUP_SCRIPT, 0, 0 );
	Application->startMainLoop();
}

JNIEnv* tTVPApplication::getJavaEnv( bool& attached ) const {
	attached = false;
	JNIEnv *env = nullptr;
	jint status = jvm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
	if( status == JNI_EDETACHED ) {
		jint astatus = jvm_->AttachCurrentThread( &env, nullptr );
		if( astatus != JNI_OK ) {
			// throw error
			TVPThrowExceptionMessage(TJS_W("Cannot attach java thread."));
			return nullptr;
		}
		attached = true;
	} else if( status != JNI_OK ) {
		TVPThrowExceptionMessage(TJS_W("Cannot retrieve java Env."));
	}
	return env;
}
void tTVPApplication::detachJavaEnv() const {
	jvm_->DetachCurrentThread();
}
static JNINativeMethod methods[] = {
		// Java側関数名, (引数の型)返り値の型, native側の関数名の順に並べます
		{ "nativeSetSurface", "(Landroid/view/Surface;)V", (void *)tTVPApplication::nativeSetSurface },
//		{ "nativeSetMessageResource", "([Ljava/lang/String;)V", (void *)tTVPApplication::nativeSetMessageResource },
		{ "nativeSetAssetManager", "(Landroid/content/res/AssetManager;)V", (void *)tTVPApplication::nativeSetAssetManager },
		{ "nativeToMessage", "(IJJ)V", (void*)tTVPApplication::nativeToMessage },
		{ "nativeSetActivity", "(Landroid/app/Activity;)V", (void *)tTVPApplication::nativeSetActivity },
		{ "nativeInitialize", "()V", (void *)tTVPApplication::nativeInitialize},
		{ "nativeOnTouch", "(IFFFIJ)V", (void *)tTVPApplication::nativeOnTouch},
		{ "nativeSetStartupPath", "(Ljava/lang/String;)V", (void *)tTVPApplication::nativeSetStartupPath},
};

jint registerNativeMethods( JNIEnv* env, const char *class_name, JNINativeMethod *methods, int num_methods ) {
	int result = 0;
	jclass clazz = env->FindClass(class_name);
	if (clazz) {
		int result = env->RegisterNatives(clazz, methods, num_methods);
		if (result < 0) {
			LOGE("registerNativeMethods failed(class=%s)", class_name);
		}
	} else {
		LOGE("registerNativeMethods: class'%s' not found", class_name);
	}
	return result;
}
#define	NUM_ARRAY_ELEMENTS(p) ((int) sizeof(p) / sizeof(p[0]))
int registerJavaMethod( JNIEnv *env)  {
	if( registerNativeMethods(env, "jp/kirikiri/krkrz/MainActivity", methods, NUM_ARRAY_ELEMENTS(methods)) < 0) {
		return -1;
	}
	return 0;
}
extern void TVPLoadMessage();
extern "C" jint JNI_OnLoad( JavaVM *vm, void *reserved ) {
	JNIEnv *env;
	if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
		return JNI_ERR;
	}
	Application = new tTVPApplication();
	Application->setJavaVM( vm );

	// register native methods
	int res = registerJavaMethod(env);

	// メッセージリソースを読み込む、strings.xmlに移動したらもう少し後で読み込んだ方がいいかもしれない
	TVPLoadMessage();
	return JNI_VERSION_1_6;
}