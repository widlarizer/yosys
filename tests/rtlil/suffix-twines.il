autoidx 1
twines
  leaf 1044 "everything.v"
  suffix 1045 1044 ":1.1-1.10"
  suffix 1046 1044 ":2.5-2.8"
  leaf 1047 "tiny"
end
srcs
  set 0 1045
  set 1 1046
  set 2 1045 1046
end
attribute \src "@2"  # everything.v:1.1-1.10|everything.v:2.5-2.8
module $pub@1047  # \tiny
  attribute \src "@0"  # everything.v:1.1-1.10
  wire input 1 \a
  attribute \src "@1"  # everything.v:2.5-2.8
  wire output 2 \b
  connect \b \a
end
