idf_component_register(SRCS "audio_doa.c" "audio_doa_tracker.c" "audio_doa_app.c"
                       INCLUDE_DIRS "." "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES esp-sr)
