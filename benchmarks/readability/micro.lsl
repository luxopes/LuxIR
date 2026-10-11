# lsl: internal-abi
load kernels as k
load storage as s

count = Int(arg(2))
repetitions = Int(arg(3))
a = s.make_set(count, 0)
b = s.make_set(count, 17)
c = s.make_set(count, 91)
mode = arg(1)
if mode == "equal":
    call k.q_f_bscopy(a, b)
elif mode == "unequal" and count > 0:
    call k.q_f_bscopy(a, b)
    call s.write_word(a, count - 1, s.read_word(a, count - 1) ~ 1)
end
checksum = 0
repeat = 0
while repeat < repetitions:
    if mode == "count":
        checksum += k.q_f_bscount(a)
    elif mode == "equal" or mode == "unequal":
        checksum += k.q_f_bsequal(a, b)
    elif mode == "bulk":
        call k.q_f_bscopy(a, b)
        call k.q_f_bsinter(a, c)
        call k.q_f_bsunion(a, b)
        call k.q_f_bsdiff(a, c)
    elif mode == "membership" and count > 0:
        call k.q_f_bsset(a, (repeat * 17) % (count * 64))
        call k.q_f_bsclr(a, (repeat * 31 + 1) % (count * 64))
    else:
        Error(ValueError: "unknown benchmark mode")
    end
    repeat += 1
end
print checksum
if mode == "bulk" or mode == "membership":
    index = 0
    while index < count:
        print s.read_word(a, index)
        index += 1
    end
end
