# The `pipeline` shell command

Utility to make building up a pipeline of shell commands easier, especially when
doing data exploration.

## installation

### MacOS

Install with [Homebrew](https://brew.sh).
```
brew tap codekitchen/pipeline
brew install pipeline
```

### From Source

```
autoreconf -fi    # if building from git, skip for release tarballs
./configure
make
```

After make finishes, you'll be able to use `./pipeline`. You can also install it using:

```
sudo make install
```
