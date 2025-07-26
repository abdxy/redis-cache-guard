# cache-guard

A lightweight Redis module that helps mitigate **cache stampede** by implementing a locking mechanism at the key level.

## 🔧 What is cache-guard?

**cache-guard** is a Redis module written in C that introduces a new command `CACHE.GUARD`. It prevents multiple concurrent processes from overwhelming the backend when a cache miss occurs by allowing only one process to rebuild the data, while others wait or return a signal.

## 💡 Motivation

While working on high-traffic systems, cache stampede was a recurring issue — especially in time-sensitive or expensive computations. I wanted to explore Redis Modules and thought this would be a good practical problem to tackle.

## 🚀 How it works

The module introduces a single command:

