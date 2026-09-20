CORE   := ../components/epet_core
CC     := clang
CFLAGS := -g -O0 -Wall -Wextra -I$(CORE) $(shell sdl2-config --cflags)
LDLIBS := $(shell sdl2-config --libs)
CORESRC := $(CORE)/epet_state.c $(CORE)/epet_render.c $(CORE)/epet_event.c $(CORE)/epet_care.c $(CORE)/epet_draw.c $(CORE)/epet_ui.c $(CORE)/epet_pages.c $(CORE)/epet_sprite.c $(CORE)/epet_species.c $(CORE)/epet_sprites.c $(CORE)/epet_module.c $(CORE)/epet_module_core.c $(CORE)/epet_store.c $(CORE)/epet_save.c $(CORE)/epet_dynpage.c $(CORE)/epet_modblob.c $(CORE)/epet_link.c $(CORE)/epet_vm.c
SRC    := main.c $(CORESRC)

epet_sim: $(SRC) $(CORE)/epet.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f epet_sim test_power test_events test_ui test_character test_modules test_save test_vm test_link *.bmp
	rm -rf epet_sim.dSYM

.PHONY: clean

TEST_SRC := ../tests/test_power.c $(CORESRC)

test_power: $(TEST_SRC) $(CORE)/epet.h
	$(CC) -g -O0 -Wall -Wextra -I$(CORE) -o $@ $(TEST_SRC)

test_events: ../tests/test_events.c $(CORESRC) $(CORE)/epet.h
	$(CC) -g -O0 -Wall -Wextra -I$(CORE) -o $@ ../tests/test_events.c $(CORESRC)

test_ui: ../tests/test_ui.c $(CORESRC) $(CORE)/epet.h
	$(CC) -g -O0 -Wall -Wextra -I$(CORE) -o $@ ../tests/test_ui.c $(CORESRC)

test_character: ../tests/test_character.c $(CORESRC) $(CORE)/epet.h
	$(CC) -g -O0 -Wall -Wextra -I$(CORE) -o $@ ../tests/test_character.c $(CORESRC)

test_modules: ../tests/test_modules.c $(CORESRC) $(CORE)/epet.h
	$(CC) -g -O0 -Wall -Wextra -I$(CORE) -o $@ ../tests/test_modules.c $(CORESRC)

test_save: ../tests/test_save.c $(CORESRC) $(CORE)/epet.h
	$(CC) -g -O0 -Wall -Wextra -I$(CORE) -o $@ ../tests/test_save.c $(CORESRC)

test_link: ../tests/test_link.c $(CORESRC) $(CORE)/epet.h
	$(CC) -g -O0 -Wall -Wextra -I$(CORE) -o $@ ../tests/test_link.c $(CORESRC)

../web/spark.epmod: ../tools/make_module.py ../tools/gen_sprites.py
	python3 ../tools/make_module.py ../web/spark.epmod

test_vm: ../tests/test_vm.c $(CORESRC) $(CORE)/epet.h
	$(CC) -g -O0 -Wall -Wextra -I$(CORE) -o $@ ../tests/test_vm.c $(CORESRC)

test: test_power test_events test_ui test_character test_modules test_save test_vm test_link ../web/spark.epmod
	./test_power
	@echo
	./test_events
	@echo
	./test_ui
	@echo
	./test_character
	@echo
	./test_modules
	@echo
	./test_save
	@echo
	./test_vm
	@echo
	./test_link ../web/spark.epmod

clean-tests:
	rm -f test_power test_events
	rm -rf test_power.dSYM test_events.dSYM

.PHONY: test clean-tests
