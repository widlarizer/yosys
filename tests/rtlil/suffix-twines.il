autoidx 1
twines
  leaf 1046 "tiny"
end
srcs
  leaf 0 "everything.v"
  suffix 1 0 ":1.1-1.10"
  suffix 2 0 ":2.5-2.8"
  set 3 1 2
end
attribute \src "@3"  # everything.v:1.1-1.10|everything.v:2.5-2.8
module $pub@1046  # \tiny
  attribute \src "@1"  # everything.v:1.1-1.10
  wire input 1 \a
  attribute \src "@2"  # everything.v:2.5-2.8
  wire output 2 \b
  connect \b \a
end
