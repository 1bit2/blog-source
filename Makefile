.PHONY: server build clean

server:
	hugo server -D --bind 0.0.0.0 --navigateToChanged

build:
	hugo --minify

clean:
	rm -rf public resources/_gen

new:
	hugo new post/$(dir)/$(name).md
	@echo "记得编辑文件补上 categories 字段"
