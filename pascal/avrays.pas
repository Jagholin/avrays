unit AVRays;

{$IFDEF FPC}
  {$MODE OBJFPC}
  {$H+}
  {$PACKRECORDS C} //выравнивание структур как в C
{$ENDIF}

interface

uses
  raylib,
  ctypes;

const
{$IFDEF MSWINDOWS}
  LIBRAVRAYS = 'avrays.dll';
{$ELSE}
  LIBRAVRAYS = 'libavrays.so';
{$ENDIF}

type

  TAVRational = record
    num: cint;
    den: cint;
  end;
  PAVRational = ^TAVRational;

  TDecoderState = cint;

  TRaylibObjects = record
    video_shader: TShader;
    tex_luma: TTexture2D;
    tex_u: TTexture2D;
    tex_v: TTexture2D;
    y_location: cint;
    u_location: cint;
    v_location: cint;
    frame_counter: cuint;
    bytespp: cuint;
  end;
  PRaylibObjects = ^TRaylibObjects;

  TMutexType = array[0..39] of Byte;
  PMutexType = ^TMutexType;
  PDecoderPrivate = Pointer;

  TTimeLine = record
    buffer: Pointer;
    elem_size: csize_t;
    cursor: cuint;
    len: csize_t;
  end;



  TDecoderContext = record
    p: PDecoderPrivate;
    dc_mutex: TMutexType;
    sample_rate: cuint;
    frame_rate: cuint;
    video_width: cuint;
    video_height: cuint;
    pixel_format: cint;

    video_tb: TAVRational;
    audio_tb: TAVRational;
    video_timest: cfloat;

    duration: cdouble;
    vbuffer_timeline: TTimeLine;
    abuffer_timeline: TTimeLine;
    abytes_pulled: culong;
    vbytes_pulled: culong;
    abytes_written: culong;
    vbytes_written: culong;
    state: TDecoderState;
  end;
  PDecoderContext = ^TDecoderContext;


const
  RESULT_ERROR = -1;
  RESULT_OK = 0;
  RESULT_STALL = 1;
  RESULT_EOF = 2;
  RESULT_CANT_OPEN = 3;
  RESULT_CHANGED = 4;


const
  DS_UNINIT = 0;
  DS_READY = 1;
  DS_STARTUP = 2;
  DS_PLAYING = 3;
  DS_FILEEOF = 4;
  DS_FINISHED = 5;
  DS_SHUTDOWN = 6;
  DS_ERROR = 100;


function avray_init_decoder(ctx: PDecoderContext): cint; cdecl; external LIBRAVRAYS;
function avray_open_file(ctx: PDecoderContext; file_name: PAnsiChar): cint; cdecl; external LIBRAVRAYS;
procedure avray_close_file(ctx: PDecoderContext); cdecl; external LIBRAVRAYS;
procedure avray_shutdown(ctx: PDecoderContext); cdecl; external LIBRAVRAYS;
function avray_is_decoder_stopped(ctx: PDecoderContext): cbool; cdecl; external LIBRAVRAYS;

function avray_pull_image(ctx: PDecoderContext; timestamp: Pcfloat): Pcuint8; cdecl; external LIBRAVRAYS;
procedure avray_release_image(ctx: PDecoderContext); cdecl; external LIBRAVRAYS;
procedure avray_seek_to_frame(ctx: PDecoderContext; ts: cdouble); cdecl; external LIBRAVRAYS;

function avray_pull_audio(ctx: PDecoderContext; audio_buffer: Pointer; frames: cuint): cint; cdecl; external LIBRAVRAYS;

function avray_init_graphics_objects(ctx: PDecoderContext; objs: PRaylibObjects): cint; cdecl; external LIBRAVRAYS;
function avray_free_graphics_objects(objs: PRaylibObjects): cint; cdecl; external LIBRAVRAYS;
function avray_update_textures(ctx: PDecoderContext; objs: PRaylibObjects): cint; cdecl; external LIBRAVRAYS;
function avray_draw_video_textures(objs: PRaylibObjects; position: TVector2; rotation: cfloat; scale_factor: cfloat; tint: TColor): cint; cdecl; external LIBRAVRAYS;

procedure avray_update_timelines(ctx: PDecoderContext); cdecl; external LIBRAVRAYS;
function time_to_str(seconds: cdouble; buf: PAnsiChar; n: csize_t): cint; cdecl; external LIBRAVRAYS;

// Функции отладки и UI
procedure timeline_draw_ui(tl: TTimeLine; x, y, width, height: cint; max: cuint; draw_background: cbool); cdecl; external LIBRAVRAYS;
function avray_draw_debug_overlay(ctx: PDecoderContext; objs: PRaylibObjects; x, y: cint; pdims: PVector2; draw_background: cbool): TVector2; cdecl; external LIBRAVRAYS;

implementation

end.
