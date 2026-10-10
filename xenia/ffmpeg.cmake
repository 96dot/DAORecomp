# FFmpeg subset (libavutil + libavcodec with Xenia's xmaframes decoder), lifted from
# xenia-canary third_party/CMakeLists.txt. Sources: third_party/FFmpeg, config: third_party/ffmpeg-xenia.
set(XE_TARGET_X86_64 TRUE)
set(XE_TARGET_AARCH64 FALSE)
# Common function to apply shared FFmpeg settings to a target.
function(ffmpeg_common target)
  target_compile_definitions(${target} PRIVATE
    HAVE_AV_CONFIG_H
    _USE_MATH_DEFINES
  )
  target_include_directories(${target} PRIVATE
    ${XE_TP}/ffmpeg-xenia
    ${XE_TP}/FFmpeg
  )

  if(WIN32)
    target_include_directories(${target} PRIVATE
      ${XE_TP}/FFmpeg/compat/atomics/win32
    )
    target_link_libraries(${target} PRIVATE bcrypt)
    # Size optimization on Debug/Checked (dead code elimination is mandatory)
    target_compile_options(${target} PRIVATE
      $<$<CONFIG:Debug>:/Os>
      $<$<CONFIG:Checked>:/Os>
    )
  else()
    target_include_directories(${target} PRIVATE
      ${XE_TP}/FFmpeg/compat/atomics/gcc
    )
  endif()

  # Suppress all warnings for FFmpeg code
  if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Xenia's generated codec/bsf lists use AVCodec* for FFCodec* (MSVC only warns).
    target_compile_options(${target} PRIVATE -w -Wno-error=incompatible-pointer-types)
  elseif(MSVC)
    target_compile_options(${target} PRIVATE /w)
  else()
    target_compile_options(${target} PRIVATE -w)
  endif()

  # All FFmpeg source is C
  set_target_properties(${target} PROPERTIES LINKER_LANGUAGE C)
endfunction()

# --- libavutil ---
add_library(libavutil STATIC
  ${XE_TP}/FFmpeg/libavutil/adler32.c
  ${XE_TP}/FFmpeg/libavutil/aes.c
  ${XE_TP}/FFmpeg/libavutil/aes_ctr.c
  ${XE_TP}/FFmpeg/libavutil/audio_fifo.c
  ${XE_TP}/FFmpeg/libavutil/avstring.c
  ${XE_TP}/FFmpeg/libavutil/avsscanf.c
  ${XE_TP}/FFmpeg/libavutil/base64.c
  ${XE_TP}/FFmpeg/libavutil/blowfish.c
  ${XE_TP}/FFmpeg/libavutil/bprint.c
  ${XE_TP}/FFmpeg/libavutil/buffer.c
  ${XE_TP}/FFmpeg/libavutil/cast5.c
  ${XE_TP}/FFmpeg/libavutil/camellia.c
  ${XE_TP}/FFmpeg/libavutil/channel_layout.c
  ${XE_TP}/FFmpeg/libavutil/csp.c
  ${XE_TP}/FFmpeg/libavutil/cpu.c
  ${XE_TP}/FFmpeg/libavutil/crc.c
  ${XE_TP}/FFmpeg/libavutil/des.c
  ${XE_TP}/FFmpeg/libavutil/dict.c
  ${XE_TP}/FFmpeg/libavutil/display.c
  ${XE_TP}/FFmpeg/libavutil/dovi_meta.c
  ${XE_TP}/FFmpeg/libavutil/downmix_info.c
  ${XE_TP}/FFmpeg/libavutil/encryption_info.c
  ${XE_TP}/FFmpeg/libavutil/error.c
  ${XE_TP}/FFmpeg/libavutil/eval.c
  ${XE_TP}/FFmpeg/libavutil/fifo.c
  ${XE_TP}/FFmpeg/libavutil/file.c
  ${XE_TP}/FFmpeg/libavutil/file_open.c
  ${XE_TP}/FFmpeg/libavutil/float_dsp.c
  ${XE_TP}/FFmpeg/libavutil/fixed_dsp.c
  ${XE_TP}/FFmpeg/libavutil/frame.c
  ${XE_TP}/FFmpeg/libavutil/hash.c
  ${XE_TP}/FFmpeg/libavutil/hdr_dynamic_metadata.c
  ${XE_TP}/FFmpeg/libavutil/hmac.c
  ${XE_TP}/FFmpeg/libavutil/hwcontext.c
  ${XE_TP}/FFmpeg/libavutil/imgutils.c
  ${XE_TP}/FFmpeg/libavutil/integer.c
  ${XE_TP}/FFmpeg/libavutil/intmath.c
  ${XE_TP}/FFmpeg/libavutil/lfg.c
  ${XE_TP}/FFmpeg/libavutil/lls.c
  ${XE_TP}/FFmpeg/libavutil/log.c
  ${XE_TP}/FFmpeg/libavutil/log2_tab.c
  ${XE_TP}/FFmpeg/libavutil/mathematics.c
  ${XE_TP}/FFmpeg/libavutil/mastering_display_metadata.c
  ${XE_TP}/FFmpeg/libavutil/md5.c
  ${XE_TP}/FFmpeg/libavutil/mem.c
  ${XE_TP}/FFmpeg/libavutil/murmur3.c
  ${XE_TP}/FFmpeg/libavutil/opt.c
  ${XE_TP}/FFmpeg/libavutil/parseutils.c
  ${XE_TP}/FFmpeg/libavutil/pixdesc.c
  ${XE_TP}/FFmpeg/libavutil/pixelutils.c
  ${XE_TP}/FFmpeg/libavutil/random_seed.c
  ${XE_TP}/FFmpeg/libavutil/rational.c
  ${XE_TP}/FFmpeg/libavutil/reverse.c
  ${XE_TP}/FFmpeg/libavutil/rc4.c
  ${XE_TP}/FFmpeg/libavutil/ripemd.c
  ${XE_TP}/FFmpeg/libavutil/samplefmt.c
  ${XE_TP}/FFmpeg/libavutil/sha.c
  ${XE_TP}/FFmpeg/libavutil/sha512.c
  ${XE_TP}/FFmpeg/libavutil/slicethread.c
  ${XE_TP}/FFmpeg/libavutil/spherical.c
  ${XE_TP}/FFmpeg/libavutil/stereo3d.c
  ${XE_TP}/FFmpeg/libavutil/threadmessage.c
  ${XE_TP}/FFmpeg/libavutil/time.c
  ${XE_TP}/FFmpeg/libavutil/timecode.c
  ${XE_TP}/FFmpeg/libavutil/tree.c
  ${XE_TP}/FFmpeg/libavutil/twofish.c
  ${XE_TP}/FFmpeg/libavutil/utils.c
  ${XE_TP}/FFmpeg/libavutil/xga_font_data.c
  ${XE_TP}/FFmpeg/libavutil/xtea.c
  ${XE_TP}/FFmpeg/libavutil/tea.c
  ${XE_TP}/FFmpeg/libavutil/tx.c
  ${XE_TP}/FFmpeg/libavutil/tx_float.c
  ${XE_TP}/FFmpeg/libavutil/tx_double.c
  ${XE_TP}/FFmpeg/libavutil/tx_int32.c
  ${XE_TP}/FFmpeg/libavutil/video_enc_params.c
  ${XE_TP}/FFmpeg/libavutil/film_grain_params.c
  ${XE_TP}/FFmpeg/libavutil/side_data.c
  ${XE_TP}/FFmpeg/libavutil/refstruct.c
  ${XE_TP}/FFmpeg/libavutil/container_fifo.c
  ${XE_TP}/FFmpeg/libavutil/float_scalarproduct.c
  ${XE_TP}/FFmpeg/libavutil/timecode_internal.c
  ${XE_TP}/FFmpeg/libavutil/iamf.c
  ${XE_TP}/FFmpeg/libavutil/timestamp.c
)
# x86 platform files
if(XE_TARGET_X86_64)
  target_sources(libavutil PRIVATE
    ${XE_TP}/FFmpeg/libavutil/x86/cpu.c
    ${XE_TP}/FFmpeg/libavutil/x86/fixed_dsp_init.c
    ${XE_TP}/FFmpeg/libavutil/x86/float_dsp_init.c
    ${XE_TP}/FFmpeg/libavutil/x86/imgutils_init.c
    ${XE_TP}/FFmpeg/libavutil/x86/lls_init.c
    ${XE_TP}/FFmpeg/libavutil/x86/tx_float_init.c
  )
endif()
# aarch64 platform files
if(XE_TARGET_AARCH64)
  target_sources(libavutil PRIVATE
    ${XE_TP}/FFmpeg/libavutil/aarch64/cpu.c
  )
  if(XE_HAS_GAS_ASM)
    # NEON init files reference symbols from the .S files, so both must
    # be included together. Requires a GAS-compatible assembler (gcc/clang).
    target_sources(libavutil PRIVATE
      ${XE_TP}/FFmpeg/libavutil/aarch64/float_dsp_init.c
      ${XE_TP}/FFmpeg/libavutil/aarch64/tx_float_init.c
    )
    xe_add_gas_sources(libavutil
      ${XE_TP}/FFmpeg/libavutil/aarch64/float_dsp_neon.S
      ${XE_TP}/FFmpeg/libavutil/aarch64/tx_float_neon.S
    )
  endif()
endif()
ffmpeg_common(libavutil)

# --- libavcodec ---
add_library(libavcodec STATIC
  ${XE_TP}/FFmpeg/libavcodec/ac3_parser.c
  ${XE_TP}/FFmpeg/libavcodec/adts_parser.c
  ${XE_TP}/FFmpeg/libavcodec/allcodecs.c
  ${XE_TP}/FFmpeg/libavcodec/avcodec.c
  ${XE_TP}/FFmpeg/libavcodec/avdct.c
  ${XE_TP}/FFmpeg/libavcodec/packet.c
  ${XE_TP}/FFmpeg/libavcodec/bitstream.c
  ${XE_TP}/FFmpeg/libavcodec/bitstream_filters.c
  ${XE_TP}/FFmpeg/libavcodec/bsf.c
  ${XE_TP}/FFmpeg/libavcodec/codec_desc.c
  ${XE_TP}/FFmpeg/libavcodec/codec_par.c
  ${XE_TP}/FFmpeg/libavcodec/d3d11va.c
  ${XE_TP}/FFmpeg/libavcodec/decode.c
  ${XE_TP}/FFmpeg/libavcodec/dirac.c
  ${XE_TP}/FFmpeg/libavcodec/dv_profile.c
  ${XE_TP}/FFmpeg/libavcodec/encode.c
  ${XE_TP}/FFmpeg/libavcodec/imgconvert.c
  ${XE_TP}/FFmpeg/libavcodec/jni.c
  ${XE_TP}/FFmpeg/libavcodec/mathtables.c
  ${XE_TP}/FFmpeg/libavcodec/mediacodec.c
  ${XE_TP}/FFmpeg/libavcodec/mpeg12framerate.c
  ${XE_TP}/FFmpeg/libavcodec/options.c
  ${XE_TP}/FFmpeg/libavcodec/parser.c
  ${XE_TP}/FFmpeg/libavcodec/parsers.c
  ${XE_TP}/FFmpeg/libavcodec/profiles.c
  ${XE_TP}/FFmpeg/libavcodec/qsv_api.c
  ${XE_TP}/FFmpeg/libavcodec/raw.c
  ${XE_TP}/FFmpeg/libavcodec/utils.c
  ${XE_TP}/FFmpeg/libavcodec/vorbis_parser.c
  ${XE_TP}/FFmpeg/libavcodec/xiph.c
  ${XE_TP}/FFmpeg/libavcodec/dct32_fixed.c
  ${XE_TP}/FFmpeg/libavcodec/dct32_float.c
  ${XE_TP}/FFmpeg/libavcodec/faandct.c
  ${XE_TP}/FFmpeg/libavcodec/faanidct.c
  ${XE_TP}/FFmpeg/libavcodec/fdctdsp.c
  ${XE_TP}/FFmpeg/libavcodec/jfdctfst.c
  ${XE_TP}/FFmpeg/libavcodec/jfdctint.c
  ${XE_TP}/FFmpeg/libavcodec/idctdsp.c
  ${XE_TP}/FFmpeg/libavcodec/simple_idct.c
  ${XE_TP}/FFmpeg/libavcodec/jrevdct.c
  ${XE_TP}/FFmpeg/libavcodec/mpegaudio.c
  ${XE_TP}/FFmpeg/libavcodec/mpegaudiodec_common.c
  ${XE_TP}/FFmpeg/libavcodec/mpegaudiodsp.c
  ${XE_TP}/FFmpeg/libavcodec/mpegaudiodsp_data.c
  ${XE_TP}/FFmpeg/libavcodec/mpegaudiodsp_fixed.c
  ${XE_TP}/FFmpeg/libavcodec/mpegaudiodsp_float.c
  ${XE_TP}/FFmpeg/libavcodec/mpegaudiodecheader.c
  ${XE_TP}/FFmpeg/libavcodec/mpegaudiodata.c
  ${XE_TP}/FFmpeg/libavcodec/sinewin.c
  ${XE_TP}/FFmpeg/libavcodec/wma_freqs.c
  ${XE_TP}/FFmpeg/libavcodec/mpegaudiodec_fixed.c
  ${XE_TP}/FFmpeg/libavcodec/mpegaudiodec_float.c
  ${XE_TP}/FFmpeg/libavcodec/wmaprodec.c
  ${XE_TP}/FFmpeg/libavcodec/wma.c
  ${XE_TP}/FFmpeg/libavcodec/wma_common.c
  ${XE_TP}/FFmpeg/libavcodec/wmadec.c
  ${XE_TP}/FFmpeg/libavcodec/aactab.c
  ${XE_TP}/FFmpeg/libavcodec/mpegaudio_parser.c
  ${XE_TP}/FFmpeg/libavcodec/pthread.c
  ${XE_TP}/FFmpeg/libavcodec/pthread_slice.c
  ${XE_TP}/FFmpeg/libavcodec/pthread_frame.c
  ${XE_TP}/FFmpeg/libavcodec/get_buffer.c
  ${XE_TP}/FFmpeg/libavcodec/vlc.c
  ${XE_TP}/FFmpeg/libavcodec/threadprogress.c
  ${XE_TP}/FFmpeg/libavcodec/bsf/null.c
  ${XE_TP}/FFmpeg/libavcodec/exif.c
  ${XE_TP}/FFmpeg/libavcodec/mpegaudiotabs.c
  ${XE_TP}/FFmpeg/libavcodec/tiff_common.c
)
if(WIN32)
  target_sources(libavcodec PRIVATE ${XE_TP}/FFmpeg/libavcodec/file_open.c)
endif()
# x86 platform files
if(XE_TARGET_X86_64)
  target_sources(libavcodec PRIVATE
    ${XE_TP}/FFmpeg/libavcodec/x86/constants.c
    ${XE_TP}/FFmpeg/libavcodec/x86/fdctdsp_init.c
    ${XE_TP}/FFmpeg/libavcodec/x86/idctdsp_init.c
    ${XE_TP}/FFmpeg/libavcodec/x86/mpegaudiodsp.c
    ${XE_TP}/FFmpeg/libavcodec/x86/fdct.c
  )
endif()
# aarch64 platform files
if(XE_TARGET_AARCH64)
  if(XE_HAS_GAS_ASM)
    target_sources(libavcodec PRIVATE
      ${XE_TP}/FFmpeg/libavcodec/aarch64/idctdsp_init_aarch64.c
      ${XE_TP}/FFmpeg/libavcodec/aarch64/mpegaudiodsp_init.c
    )
    xe_add_gas_sources(libavcodec
      ${XE_TP}/FFmpeg/libavcodec/aarch64/idctdsp_neon.S
      ${XE_TP}/FFmpeg/libavcodec/aarch64/mpegaudiodsp_neon.S
      ${XE_TP}/FFmpeg/libavcodec/aarch64/simple_idct_neon.S
    )
  endif()
endif()
target_include_directories(libavcodec PRIVATE
  ${XE_TP}/FFmpeg/libavcodec
)
target_link_libraries(libavcodec PRIVATE libavutil)
ffmpeg_common(libavcodec)
