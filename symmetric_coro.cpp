#include <utility>

#include <iostream>
#include <assert.h>

#define __CPS  __attribute__((annotate("CPS_function")))

using namespace std;

namespace std {
///////////////////////////////////////////////////////////
// Low level CPS support

class cps_target {
protected:
  struct cps_arg {
    cps_arg() : data(nullptr) {}
    cps_arg(int i) : i(i) {}
    cps_arg(float f) : f(f) {}
    cps_arg(double d) : d(d) {}

    operator int() { return i; }
    operator float() { return f; }
    operator double() { return d; }

    template <typename T> operator T() { return *(reinterpret_cast<T*>(data)); }

    union {
      int i;
      float f;
      double d;
      void *data;
    };
  };

  // Packs a continuation and type-erased data
  struct cps_call_data {
    cps_arg data;
    cps_target* cont;
  };

  // This trampoline simulates tail calls
  static cps_arg trampoline(cps_target* target, cps_arg arg) {
    assert(target != nullptr);

    cps_target* callee = target;
    cps_arg data = arg;
    cps_target* cont = nullptr;

    while (callee != nullptr) {
      cps_call_data call_data = callee->__body({data, cont});

      cont = callee;
      callee = call_data.cont;
      data = call_data.data;
    }

    return data;
  }

  // The coroutine body and current suspend point
  virtual __CPS cps_call_data __body(cps_call_data call_data) = 0;
};

template<class...> class coroutine;
template<class...> class resume_continuation;

///////////////////////////////////////////////////////////
// Resume continuation implementation

template<> class resume_continuation<> : public cps_target {
public:
  bool is_valid() const {
    return _target != this;
  }

protected:
  resume_continuation()
    : _helper(*this)
    , _target(this) // An invalidated continuation points to itself
  {}                // because nullptr is a valid target.

  resume_continuation(cps_target *target)
    : _helper(*this)
    , _target(target)
  {}

  resume_continuation& operator=(resume_continuation&& other) {
    if (other.is_valid()) {
      _target = other._target;
    } else {
        _target = this;
    }
    return *this;
  }

  cps_arg call_with_trampoline() {
    return trampoline(this, {});
  }

  template<typename A>
  cps_arg call_with_trampoline(A arg) {
    return trampoline(this, {arg});
  }

  cps_call_data get_call_data() {
    return {{}, this};
  }

  template<typename A>
  cps_call_data get_call_data(A arg) {
    return {{arg}, this};
  }

private:
  // A helper class
  class internal : public cps_target {
  public:
    internal(resume_continuation<>& owner)
      : _owner(owner)
    {}

    __CPS cps_call_data __body(cps_call_data call_data) override {
      // If the owner is valid, the internal continuation was called
      // by the owner to transfer control to its target. Otherwise,
      // someone is trying to give control back to the owner.
      if (_owner.is_valid()) {
        cps_target *target = _owner._target;
        _owner._target = &_owner;
        return {call_data.data, target};
      } else {
        _owner._target = call_data.cont;
        return {call_data.data, &_owner};
      }
  };
  private:
    resume_continuation<>& _owner;
  };

  __CPS cps_call_data __body(cps_call_data call_data) override {
    if (call_data.cont != &_helper) {
      // If transfer doesn't come from the helper, remember the caller and
      // use the helper to transfer control to the target. The helper will
      // invalidate this continuation.
      assert(_target != this && "Invoking an invalidated resume continuation");
      _caller = call_data.cont;
      return {call_data.data, &_helper};
    } else {
      // If transfer comes from the helper, just pass control on to
      // the saved caller. The helper has updated the target.
      return {call_data.data, _caller};
    }
  }

  internal _helper;
  cps_target* _target;
  cps_target* _caller;
};

///////////////////////////////////////////////////////////
// End-user resume continuation - type-safe wrappers on top
// of the type-erased one.

template<> class resume_continuation<void(void)> : public resume_continuation<> {
public:
  resume_continuation()
    : resume_continuation<>()
  {}

  resume_continuation(cps_target* target)
    : resume_continuation<>(target)
  {}

  resume_continuation(coroutine<void()> *c);

  resume_continuation<void(void)>& operator=(resume_continuation<void(void)>&& other) {
    resume_continuation<>::operator=(std::move(other));
    return *this;
  }


  void call_with_trampoline() {
    resume_continuation<>::call_with_trampoline();
  }

  cps_call_data get_call_data() {
    return resume_continuation<>::get_call_data();
  }
};

template<class R> class resume_continuation<R(void)> : public resume_continuation<> {
public:
  resume_continuation()
    : resume_continuation<>()
  {}

  resume_continuation(cps_target* target)
    : resume_continuation<>(target)
  {}

  resume_continuation(coroutine<R()> *c);

  R call_with_trampoline() {
    return resume_continuation<>::call_with_trampoline();
  }

  cps_call_data get_call_data() {
    return resume_continuation<>::get_call_data();
  }
};

template<class A> class resume_continuation<void(A)> : public resume_continuation<> {
public:
  resume_continuation()
    : resume_continuation<>()
  {}

  resume_continuation(cps_target* target)
    : resume_continuation<>(target)
  {}

  resume_continuation(coroutine<void(A)> *c);

  void call_with_trampoline(A arg) {
    return resume_continuation<>::call_with_trampoline(arg);
  }

  cps_call_data get_call_data(A arg) {
    return resume_continuation<>::get_call_data(arg);
  }
};

template<class R, class A> class resume_continuation<R(A)> : public resume_continuation<> {
public:
  resume_continuation()
    : resume_continuation<>()
  {}

  resume_continuation(cps_target* target)
    : resume_continuation<>(target)
  {}

  resume_continuation(coroutine<R(A)> *c);

  R call_with_trampoline(A arg) {
    return resume_continuation<>::call_with_trampoline(arg);
  }

  cps_call_data get_call_data(A arg) {
    return resume_continuation<>::get_call_data(arg);
  }
};

///////////////////////////////////////////////////////////
// Base coroutine class - stores just the suspend point.

template<> class coroutine<> : public cps_target {
public:
  bool done() const {
      return _sp == -1;
  }

protected:
  using suspend_point = int;

  coroutine()
    : _sp(0)
  {}

  suspend_point get_suspend_point() const {
    return _sp;
  }

  void set_suspend_point(suspend_point sp) {
    assert(!done());
    _sp = sp;
  }

  void set_done() {
    _sp = -1;
  }

private:
  suspend_point _sp;
};

///////////////////////////////////////////////////////////
// Type-safe coroutine classes for user coroutines - add the
// two resume continuations.

template<> class coroutine<void(void)> : public coroutine<> {
public:
  // Not implemented on purpose
  void __CPS operator()();

  // Calling `coroutine<void(void)>::operator()` from a non-cps
  // function translates to `call_with_trampoline()`.
  void call_with_trampoline() {
    return _cont.call_with_trampoline();
  }

  // Calling `coroutine<void(void)>::operator()` from a cps function
  // translates to `return get_call_data();`.
  cps_call_data get_call_data() {
    return _cont.get_call_data();
  }

protected:
  coroutine()
    : _cont(this)
    , _caller()
  {}

  // Translates to `get_caller()()`
  void __CPS yield();

  void set_caller(resume_continuation<void(void)>&& caller) {
    _caller = std::move(caller);
  }

  resume_continuation<void(void)>& get_caller() {
    return _caller;
  }

private:
  resume_continuation<void(void)> _cont;
  resume_continuation<void(void)> _caller;
};

resume_continuation<void()>::resume_continuation(coroutine<void()> *c)
  : resume_continuation<>(static_cast<coroutine<>*>(c))
{}


template<class R> class coroutine<R(void)> : public coroutine<> {
public:
  // Not implemented on purpose
  void __CPS operator()();

  // Calling `coroutine<R(void)>::operator()` from a non-cps
  // function translates to `_cont.call_with_trampoline()`.
  R call_with_trampoline() {
    return _cont.call_with_trampoline();
  }

  // Calling `coroutine<R(void)>::operator()` from a cps function
  // translates to `return get_call_data();`.
  cps_call_data get_call_data() {
    return _cont.get_call_data();
  }

protected:
  coroutine()
    : _cont(this)
    , _caller()
  {}

  // Translates to `get_caller()(result)`
  void __CPS yield(R result);

  void set_caller(resume_continuation<void(R)>&& caller) {
    _caller = std::move(caller);
  }

  resume_continuation<void(R)>& get_caller() {
    return _caller;
  }

private:
  resume_continuation<R(void)> _cont;
  resume_continuation<void(R)> _caller;
};

template<class R>
resume_continuation<R()>::resume_continuation(coroutine<R()> *c)
  : resume_continuation<>(static_cast<coroutine<>*>(c))
{}

template<class A> class coroutine<void(A)> : public coroutine<> {
public:
  // Not implemented on purpose
  void __CPS operator()();

  // Calling `coroutine<void(A)>::operator(arg)` from a non-cps
  // function translates to `call_with_trampoline(arg)`.
  void call_with_trampoline(A arg) {
    return _cont.call_with_trampoline(arg);
  }

  // Calling `coroutine<void(A)>::operator(A)` from a cps function
  // translates to `return get_call_data();`.
  cps_call_data get_call_data(A arg) {
    return _cont.get_call_data(arg);
  }

protected:
  coroutine()
    : _cont(this)
    , _caller()
  {}

  // Translates to `A arg = get_caller()()`
  void __CPS yield();

  void set_caller(resume_continuation<A(void)>&& caller) {
    _caller = std::move(caller);
  }

  resume_continuation<A(void)>& get_caller() {
    return _caller;
  }

  void set_initial_value(A&& value) {
    new (&_initial_value) A(value);
  }

  const A& get_initial_value() const {
    return _initial_value;
  }

private:
  resume_continuation<void(A)> _cont;
  resume_continuation<A(void)> _caller;

  union {
    A _initial_value;
  };
};

template<class A>
resume_continuation<void(A)>::resume_continuation(coroutine<void(A)> *c)
  : resume_continuation<>(static_cast<coroutine<>*>(c))
{}

template<class R, class A> class coroutine<R(A)> : public coroutine<> {
public:
  // Not implemented on purpose
  R __CPS operator()(A);

  // Calling `coroutine<R(A)>::operator()` from a non-cps
  // function translates to `call_with_trampoline(arg)`.
  R call_with_trampoline(A arg) {
    return _cont.call_with_trampoline(arg);
  }

  // Calling `coroutine<R(A)>::operator()` from a cps function
  // translates to `return get_call_data(A);`.
  cps_call_data get_call_data(A arg) {
    return _cont.get_call_data(arg);
  }

protected:
  coroutine()
    : _cont(this)
    , _caller()
  {}

  // Translates to `A arg = get_caller()(result)`
  A __CPS yield(R result);

  void set_caller(resume_continuation<A(R)>&& caller) {
    _caller = std::move(caller);
  }

  resume_continuation<A(R)>& get_caller() {
    return _caller;
  }

  void set_initial_value(A&& value) {
    new (&_initial_value) A(value);
  }

  const A& get_initial_value() const {
    return _initial_value;
  }

private:
  resume_continuation<R(A)> _cont;
  resume_continuation<A(R)> _caller;

  union {
    A _initial_value;
  };
};


template<class R, class A>
resume_continuation<R(A)>::resume_continuation(coroutine<R(A)> *c)
  : resume_continuation<>(static_cast<coroutine<>*>(c))
{}

};


/// Example one: a coroutine that yields once. Demonstrates the difference
/// between yielding and returning.
///
/// Should be able to be called exactly twice.

/*
yield_once() : coroutine<void(void)>
{
  yield(); // get_caller()();
  return;
}
*/

// Translates to
class yield_once : public coroutine<void(void)>
{
public:
    yield_once()
    {}

private:
    struct corotine_state {
    };

    __CPS cps_call_data __body(cps_call_data call_data) override
    {
        switch (get_suspend_point())
        {
        case 0: // initial suspend point
            // Save caller
            set_caller(resume_continuation<void(void)>(call_data.cont));

            // yield()
            set_suspend_point(1);
            return get_caller().get_call_data();

        case 1: // suspend point 1
            // return
            set_done();
            return get_caller().get_call_data();;

        default:
            assert(false && "Called a completed coroutine");
            return {};
        };
    }
};

void test_yield_once()
{
    cout << "*** Test yield_once ***" << endl;
    yield_once yo;

    assert(!yo.done());

    yo.call_with_trampoline(); // yo();
    assert(!yo.done());

    yo.call_with_trampoline(); // yo();
    assert(yo.done());
}

/// Example two: a coroutine that prints a sequence of numbers. The start value
/// and the step are passed to the coroutine constructor. Demonstrates coroutine
/// creation arguments.
///
/// Should be able to be called infinitely many times.

/*
print_counter(int start, int step) : coroutine<void(void)> {
  for (int i = start; ; i += step) {
    cout << i << endl;
    yield();
  }
}
*/

// Translates to:
class print_counter : public coroutine<void(void)>
{
public:
    print_counter(int start, int step)
        : start(start)
        , step(step)
    {}

private:
    struct coroutine_state {
        union { int i; };
    } __state;

    __CPS cps_call_data __body(cps_call_data call_data) override
    {
        switch (get_suspend_point())
        {
        case 0: // initial suspend point
            // Save caller
            set_caller(resume_continuation<void(void)>(call_data.cont));

            new (&__state.i) int(0);
            for (;; __state.i += step) {
                cout << __state.i << endl;

                set_suspend_point(1);
                return get_caller().get_call_data();
        case 1: // suspend point
                continue;
            }

        default:
            assert(false && "Called a completed coroutine");
            return {};
        };
    }

    int start;
    int step;
};

void test_print_counter()
{
    cout << "*** Test print_counter ***" << endl;
    print_counter pc(1, 3);

    assert(!pc.done());

    for (int i = 0; i < 4; ++i) {
        pc.call_with_trampoline();
        assert(!pc.done());
    }

    assert(!pc.done());
}

/// Example three: a coroutine which returns a range of numbers. The start
/// and end value are passed to the coroutine constructor. Demonstrates how
/// the coroutine produces values.
///
/// Should be able to be called (start - end) times.

/*
range(int start, int end) : coroutine<int()>
{
  for (int i = start; i < end - 1; ++i) {
    yield(i);
  }

  return end - 1;
}
*/

// Translates to:
class range : public coroutine<int(void)>
{
public:
    range(int start, int end)
        : start(start)
        , end(end)
    {}

private:
    struct coroutine_state {
        union { int i; };
    } __state;

    __CPS cps_call_data __body(cps_call_data call_data) override
    {
        switch (get_suspend_point())
        {
        case 0: // initial suspend point
            set_caller(resume_continuation<void(int)>(call_data.cont));

            for (new (&__state.i) int(start);
                 __state.i < end - 1;
                 ++__state.i) {
                set_suspend_point(1);
                return get_caller().get_call_data(__state.i);
        case 1: // suspend point 1
                continue;
            }

            set_done();
            return get_caller().get_call_data(end - 1);

        default:
            assert(false && "Called a completed coroutine");
            return {};
        };
    }

    int start;
    int end;
};

void test_range()
{
    cout << "*** Test range ***" << endl;
    const int start = 10;
    const int end = 14;
    range r(start, end);

    for (int i = start; i < end; ++i) {
        assert(!r.done());
        int val = r.call_with_trampoline();
        cout << val << endl;
        assert(val == i);
    }

    assert(r.done());
}

/// Example four: a coroutine which returns the values passed to it. Demonstrates
/// how a coroutine consumes values.
///
/// Should be able to be called infinitely many times.

/*
echo() : coroutine<int(int)>
{
  int val = get_initial_value();

  for (;;) {
    val = yield(val);
  }
}
*/

// Translates to:
class echo : public coroutine<int(int)>
{
public:
    echo()
    {}

private:
    struct coroutine_state {
        union { int val; };
    } __state;

    __CPS cps_call_data __body(cps_call_data call_data) override
    {
        switch (get_suspend_point())
        {
        case 0: // initial suspend point
            set_caller(resume_continuation<int(int)>(call_data.cont));
            set_initial_value(static_cast<int>(call_data.data));

            new (&__state.val) int(get_initial_value());

            for (;;) {
                set_suspend_point(1);
                return get_caller().get_call_data(__state.val);
        case 1:
                __state.val = static_cast<int>(call_data.data);
            }

        default:
            assert(true && "Called a completed coroutine");
            return {};
        };
    }
};

void test_echo()
{
    cout << "*** Test echo ***" << endl;
    echo e;

    assert(!e.done());

    for (int i = 0; i < 4; ++i) {
        int response = e.call_with_trampoline(i);
        cout << i << " -> " << response << endl;
        assert(response == i);
        assert(!e.done());
    }

    assert(!e.done());
}

/// Example five: a coroutine which consumes values from two range coroutines
/// (provided by the caller) and returns the products of the values. Demonstrates
/// control flow between coroutines.

/*
multiply(range& r1, range& r2) : coroutine<int()>
{
  assert(!r1.done() && !r2.done());

  for(;;) {
    int result = yield(r1() * r2());

    if (!r1.done() && !r2.done())
      yield(result);
    else
      return result;
  }
}
*/

// Translates to:
class multiply : public coroutine<int()> {
public:
    multiply(range& r1, range& r2)
        : r1(r1)
        , r2(r2)
    {}

private:
    struct coroutine_state {
        union { int _temp1; };
        union { int _temp2; };
        union { int result; };
    } __state;

    __CPS cps_call_data __body(cps_call_data call_data) override
    {
        switch (get_suspend_point())
        {
        case 0:
            set_caller(resume_continuation<void(int)>(call_data.cont));

            assert(!r1.done() && !r2.done());

            for (;;) {
                // _temp1 = r1();
                set_suspend_point(1);
                return r1.get_call_data();
        case 1:
                new (&__state._temp1) int(call_data.data);

                // _temp2 = r2();
                set_suspend_point(2);
                return r2.get_call_data();
        case 2:
                new (&__state._temp2) int(call_data.data);

                // result = temp1 * temp2;
                new (&__state.result) int(__state._temp1 * __state._temp2);

                if (!r1.done() && !r2.done()) {
                    // yield(result);
                    set_suspend_point(3);
                    return get_caller().get_call_data(__state.result);
        case 3:
                    continue;
                } else {
                    // yield(result);
                    set_done();
                    return get_caller().get_call_data(__state.result);
                }
            }

        default:
            assert(true && "Called a completed coroutine");
            return {};
        };
    }

    range& r1;
    range& r2;
};

void test_multiply()
{
    cout << "*** Test mutiply ***" << endl;
    range r1(0, 4);
    range r2(0, 10);

    multiply m(r1, r2);

    assert(!m.done());

    while (!m.done()) {
        assert(!r1.done());
        assert(!r2.done());

        int product = m.call_with_trampoline();
        cout << product << endl;
    }

    assert(r1.done());
    assert(!r2.done());
}

int main()
{
    test_yield_once();
    test_print_counter();
    test_range();
    test_echo();
    test_multiply();

    return 0;
}
