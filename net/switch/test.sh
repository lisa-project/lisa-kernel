#!/bin/bash

swctl add eth3
swctl add eth4
swctl setportvlan eth3 11
swctl addportvlan eth4 11
swctl settrunk eth4 1
swctl addvlan 11 teste
