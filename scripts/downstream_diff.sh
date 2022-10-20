#!/usr/bin/env bash

skips="cros"

comm -1 <(git ls-tree --name-only cros/master | sort) \
  <(git ls-tree --name-only cros/upstream_validation | sort) \
  | grep -vf <(echo "${skips}") \
  | xargs git diff cros/master cros/upstream_validation --stat -- \
  | cat

# writeprotect.c                                 |   18 +-
# 96 files changed, 1887 insertions(+), 9391 deletions(-)
