# Style Guide

## Naming

- **snake_case**: everything — variables, functions, types, files, namespaces
- **CamelCase**: concept names and template parameters only

## Initialization

Always use `{}`, never `()`

## Aggregate Initialization

Always use designated initializers for aggregate types

## Formatting

Pointers and references bind to the type, not the name (`.clang-format`: `PointerAlignment: Left`)

