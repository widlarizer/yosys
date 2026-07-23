autoidx 1
twines
  leaf 1046 "everything.v"
  suffix 1047 1046 ":1.1-1.10"
  suffix 1048 1046 ":2.5-2.8"
  concat 1049 1047 1048
  leaf 1050 "tiny"
end
attribute \src "@1049"  # everything.v:1.1-1.10|everything.v:2.5-2.8
module $pub@1050  # \tiny
  attribute \src "@1047"  # everything.v:1.1-1.10
  wire input 1 \a
  attribute \src "@1048"  # everything.v:2.5-2.8
  wire output 2 \b
  connect \b \a
end
