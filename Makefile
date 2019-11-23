all:
	clang $(PIPELINE_FLAGS) -o pipeline -lreadline -lncurses main.c
