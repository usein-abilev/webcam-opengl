CC = gcc
CFLAGS = -ggdb -std=c11 -pedantic -Wall -Wextra
CLIBS = $(shell pkg-config --cflags --libs glfw3 libv4l2) -lm
OUT_DIR = build

TARGET = camgl
SRCS = main.c shader.c glad.c 
OBJS = $(addprefix $(OUT_DIR)/, $(SRCS:.c=.o))

all: $(OUT_DIR) $(TARGET)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(OUT_DIR)/$(TARGET) $(CLIBS) 

$(OUT_DIR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean: 
	rm -rf $(OUT_DIR)

.PHONY: all clean
