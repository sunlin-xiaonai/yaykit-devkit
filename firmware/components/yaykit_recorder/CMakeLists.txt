# Recordings on the TF card (docs/architecture/audio.md). Pure C over stdio/POSIX, host tested with test/run.sh.
idf_component_register(SRCS "yk_rec.c"
    INCLUDE_DIRS ".")
