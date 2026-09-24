(Bench threading test: requires configured G33 and valid spindle feedback)
(Spindle start is deliberately an operator/setup step)
G18 G90 G94 G21
G7
G0 X40 Z0
G76 X36 Z-20 P2 Q1 F1.5 L1
