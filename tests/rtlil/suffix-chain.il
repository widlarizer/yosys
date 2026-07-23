autoidx 1
twines
  leaf 1046 "/home/emil/repo/"
  suffix 1047 1046 "foo/"
  suffix 1048 1047 "bar.v"
  suffix 1049 1048 ":10.1-10.5"
  suffix 1050 1048 ":11.1-11.5"
end
module \chain
  attribute \src "@1049"  # /home/emil/repo/foo/bar.v:10.1-10.5
  wire input 1 \a
  attribute \src "@1050"  # /home/emil/repo/foo/bar.v:11.1-11.5
  wire output 2 \b
  connect \b \a
end
