# CLN's int constructors pack their argument straight into a fixnum with no
# range check, which is only valid when every int fits one (cl_value_len >= 32).
# On a 32-bit target cl_value_len is 30, so values with bit 29 set are silently
# corrupted (1000000007 became -73741817, 1<<30 became 0). The long and
# unsigned long constructors already range-check, so make the int ones delegate
# to them. The guard keeps this inert on a 64-bit target.
#
# Usage: awk -f cln-int-ctor-patch.awk include/cln/number.h > number.h.new
#
# Rewrites the two macro definitions in place; prints the patched file.

BEGIN { inblk = "" }

# The int constructor macro. Wrap the original definition in a guard; the
# original line itself is printed by the fall-through at the end.
# (Do not "next" here -- the #define has to be kept.)
/^#define CL_DEFINE_INT_CONSTRUCTORS\(_class_\)/ {
    print "#if (cl_value_len >= 32)"
    print "// Every int fits into a fixnum value; the packed form is correct."
    inblk = "ctor"
}

/^#define CL_DEFINE_INT_ASSIGNMENT_OPERATORS\(_class_\)/ {
    print "#if (cl_value_len >= 32)"
    inblk = "assign"
}

# The last line of either block: keep it, close the guard, and add the
# delegating version for targets where an int does not fit a fixnum.
inblk != "" && /^CL_DEFINE_INT_(CONSTRUCTOR|ASSIGNMENT_OPERATOR)\(_class_, unsigned int\)/ {
    print
    print "#else"
    print "// CLNFIX: cl_value_len is 30 on a 32-bit target, so an int does not always"
    print "// fit a fixnum (bit 29 set, and unsigned >= 2^29, do not). Delegate to the"
    print "// long / unsigned long constructors, which range-check and build a bignum"
    print "// when needed."
    if (inblk == "ctor") {
        print "#define CL_DEFINE_INT_CONSTRUCTORS(_class_)  \\"
        print "inline _class_::_class_ (const int wert) : _class_ ((long) wert) {}   \\"
        print "inline _class_::_class_ (const unsigned int wert) : _class_ ((unsigned long) wert) {}"
    } else {
        print "#define CL_DEFINE_INT_ASSIGNMENT_OPERATORS(_class_)  \\"
        print "inline _class_& _class_::operator= (const int wert) { return *this = (long) wert; }   \\"
        print "inline _class_& _class_::operator= (const unsigned int wert) { return *this = (unsigned long) wert; }"
    }
    print "#endif"
    inblk = ""
    next
}

{ print }
