autoidx 1
twines
  leaf 1046 "everything.v"
  suffix 1047 1046 ":1.1-1.10"
  suffix 1048 1046 ":2.5-2.8"
  leaf 1049 "tiny"
end
srcs
  set 0 1047
  set 1 1048
  set 2 1047 1048
end
attribute \src "@2"  # everything.v:1.1-1.10|everything.v:2.5-2.8
module $pub@1049  # \tiny
  attribute \src "@0"  # everything.v:1.1-1.10
  wire input 1 \a
  attribute \src "@1"  # everything.v:2.5-2.8
  wire output 2 \b
  connect \b \a
end
