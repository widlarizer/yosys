autoidx 1
twines
  leaf 1044 "/home/emil/repo/"
  suffix 1045 1044 "foo/"
  suffix 1046 1045 "bar.v"
  suffix 1047 1046 ":10.1-10.5"
  suffix 1048 1046 ":11.1-11.5"
end
srcs
  set 0 1047
  set 1 1048
end
module \chain
  attribute \src "@0"  # /home/emil/repo/foo/bar.v:10.1-10.5
  wire input 1 \a
  attribute \src "@1"  # /home/emil/repo/foo/bar.v:11.1-11.5
  wire output 2 \b
  connect \b \a
end
