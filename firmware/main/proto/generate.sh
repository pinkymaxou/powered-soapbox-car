#!/bin/sh
# Régénère kart.pb.c / kart.pb.h depuis kart.proto (générateur nanopb + protoc).
# Debian/Ubuntu : apt install nanopb protobuf-compiler
cd "$(dirname "$0")"
nanopb_generator kart.proto
