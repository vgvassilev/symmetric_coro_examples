#include <assert.h>
#include <memory>
#include <utility>

extern "C" int printf(const char*, ...);

using namespace std;

namespace std {
///////////////////////////////////////////////////////////
// Low level CPS support

class cps_target {
public:
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
  static cps_call_data trampoline(cps_target* target, cps_arg arg) {
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

    return {data, cont};
  }

  // The coroutine body and current suspend point
  virtual cps_call_data __body(cps_call_data call_data) = 0;
};

template<class... Ts> class coroutine;
template<class... Ts> class resume_continuation;


///////////////////////////////////////////////////////////
// Resume continuation implementation

template<> class coroutine<>;

template<> class resume_continuation<> {
  friend class coroutine<>;

public:
  bool is_valid() const {
    return _target != get_invalid_continuation();
  }

protected:
  resume_continuation()
    : _target(get_invalid_continuation()) // An invalidated continuation points to itself
  {}                // because nullptr is a valid target.

  resume_continuation(cps_target *target)
    : _target(target)
  {}

  resume_continuation& operator=(resume_continuation&& other) {
    if (other.is_valid()) {
      _target = other._target;
      other.invalidate();
    } else {
      invalidate();
    }

    return *this;
  }

  cps_target* release() {
    cps_target* target = _target;
    invalidate();
    return target;
  }

  void reset(cps_target* new_target) {
    _target = new_target;
  }

  cps_target::cps_arg call_with_trampoline() {
    cps_target::cps_call_data call_data = cps_target::trampoline(release(), {});
    reset(call_data.cont);
    return call_data.data;
  }

  template<typename A>
  cps_target::cps_arg call_with_trampoline(A arg) {
    cps_target::cps_call_data call_data = cps_target::trampoline(release(), {arg});
    reset(call_data.cont);
    return call_data.data;
  }

private:
  class invalid_cps_target : public cps_target {
    cps_call_data __body(cps_call_data call_data) override {
      assert(false && "Invoked invalid continuation");
      return {{}, nullptr};
    }
  };

  static cps_target* get_invalid_continuation() {
    static invalid_cps_target invalid_continuation;
    return &invalid_continuation;
  }

  void invalidate() {
    _target = get_invalid_continuation();
  }

  // Should we have this at all??? Should the continuation be a cps_target? I think not...
  // Maybe instead we should have a helper cps_target for the call with a trampoline.
  cps_target* _target;
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

  void operator()() {
    call_with_trampoline();
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

  R operator()() {
    return call_with_trampoline();
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

  void operator()(A arg) {
    call_with_trampoline(arg);
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

  R operator()(A arg) {
    return call_with_trampoline(arg);
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
  // Inside a coroutine body, some invocations get rewritten as follows:
  //
  //   1) `coroutine::yield()`   >>>    `get_caller()()`
  //
  //   2) `coro()` >>> `coro._cont()`
  //
  //   3) `rc()`, where `rc` is a `resume_continuation`   >>>
  //      ```
  //        prepare_to_suspend(N, rc);
  //      case N:
  //        process_resume(rc, call_data)
  //      ```
  //      where `N` is a generated id for the suspend point, unique within this
  //      coroutine body.

  using suspend_point = int;

  coroutine()
    : _sp(0)
  {}

  suspend_point get_suspend_point() const {
    return _sp;
  }

  cps_call_data prepare_to_suspend(suspend_point sp, resume_continuation<>& cont) {
    _sp = sp;
    return {{}, cont.release()};
  }

  template<typename ValType>
  cps_call_data prepare_to_suspend(suspend_point sp, resume_continuation<>& cont, ValType val) {
    _sp = sp;
    return {{val}, cont.release()};
  }

  void process_resume(resume_continuation<>& cont, cps_call_data& call_data) {
    cont.reset(call_data.cont);
  }

  template<typename ValType>
  ValType process_resume(resume_continuation<>& cont, cps_call_data& call_data) {
    cont.reset(call_data.cont);
    return call_data.data;
  }

  constexpr static suspend_point _sp_done = -1;

private:
  suspend_point _sp;
};

///////////////////////////////////////////////////////////
// Type-safe coroutine classes for user coroutines - add the
// two resume continuations.

template<> class coroutine<void(void)> : public coroutine<> {
public:
 // Always inlined in non-coroutines, injected in coroutine bodies
  void operator()() {
    _cont();
  }

  auto& get_cont() { return _cont; }

protected:
  coroutine()
    : _cont(this)
    , _caller()
  {}

  // Always inlined
  void yield() {
    get_caller()();
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
 // Always inlined in non-coroutines, injected in coroutine bodies
  R operator()() {
    return _cont();
  }

  auto& get_cont() { return _cont; }

protected:
  coroutine()
    : _cont(this)
    , _caller()
  {}

  // Always inlined
  void yield(R result) {
    get_caller()(result);
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
 // Always inlined in non-coroutines, injected in coroutine bodies
  void operator()(A arg) {
    _cont(arg);
  }

  auto& get_cont() { return _cont; }

protected:
  coroutine()
    : _cont(this)
    , _caller()
  {}

  // Always inlined
  A yield() {
    return get_caller()();
  }

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
  // Always inlined in non-coroutines, injected in coroutine bodies
  R operator()(A arg) {
    return _cont(arg);
  }

  auto& get_cont() { return _cont; }

protected:
  coroutine()
    : _cont(this)
    , _caller()
  {}

  // Always inlined
  A yield(R result) {
    return get_caller()(result);
  }

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

    cps_call_data __body(cps_call_data call_data) override
    {
        switch (get_suspend_point())
        {
        case 0:
            // Initial suspend point - save caller
            process_resume(get_caller(), call_data);

            // yield() == get_caller()() == expansion of suspend_to(get_caller()())
            return prepare_to_suspend(1, get_caller());

        case 1: // suspend point 1
            process_resume(get_caller(), call_data);

            // return
            return prepare_to_suspend(_sp_done, get_caller());

        default:
            assert(false && "Called a completed coroutine");
            return {};
        };
    }
};

void test_yield_once()
{
    printf("*** Test yield_once ***\n");
    yield_once yo;

    assert(!yo.done());

    yo();
    assert(!yo.done());

    yo(); // yo();
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
    printf("%d\n, i);
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

    cps_call_data __body(cps_call_data call_data) override
    {
        switch (get_suspend_point())
        {
        case 0:
            // Initial suspend point - Save caller
            process_resume(get_caller(), call_data);

            new (&__state.i) int(start);
            for (;; __state.i += step) {
                printf("%d\n", __state.i);

                return prepare_to_suspend(1, get_caller());
        case 1: // suspend point
                process_resume(get_caller(), call_data);
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
    printf("*** Test print_counter ***\n");
    print_counter pc(1, 3);

    assert(!pc.done());

    for (int i = 0; i < 4; ++i) {
        pc();
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

    cps_call_data __body(cps_call_data call_data) override
    {
        switch (get_suspend_point())
        {
        case 0: // initial suspend point
            process_resume(get_caller(), call_data);

            for (new (&__state.i) int(start);
                 __state.i < end - 1;
                 ++__state.i) {
                return prepare_to_suspend(1, get_caller(), __state.i);
        case 1: // suspend point 1
                process_resume(get_caller(), call_data);
            }

            return prepare_to_suspend(_sp_done, get_caller(), end - 1);

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
    printf("*** Test range ***\n");
    const int start = 10;
    const int end = 14;
    range r(start, end);

    for (int i = start; i < end; ++i) {
        assert(!r.done());
        int val = r();
        printf("%d\n", val);
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

    cps_call_data __body(cps_call_data call_data) override
    {
        switch (get_suspend_point())
        {
        case 0: // initial suspend point - saves the caller and the initial value
            set_initial_value(process_resume<int>(get_caller(), call_data));

            new (&__state.val) int(get_initial_value());

            for (;;) {
                return prepare_to_suspend(1, get_caller(), __state.val);

        case 1:
                __state.val = process_resume<int>(get_caller(), call_data);
            }

        default:
            assert(true && "Called a completed coroutine");
            return {};
        };
    }
};

void test_echo()
{
    printf("*** Test echo ***\n");
    echo e;

    assert(!e.done());

    for (int i = 0; i < 4; ++i) {
        int response = e(i);
        printf("%d -> %d\n", i, response);
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

    cps_call_data __body(cps_call_data call_data) override
    {
        switch (get_suspend_point())
        {
        case 0:
            process_resume(get_caller(), call_data);

            assert(!r1.done() && !r2.done());

            for (;;) {
                // _temp1 = r1();
                return prepare_to_suspend(1, r1.get_cont());
        case 1:
                new (&__state._temp1) int(process_resume<int>(r1.get_cont(), call_data));

                // _temp2 = r2();
                return prepare_to_suspend(2, r2.get_cont());
        case 2:
                new (&__state._temp2) int(process_resume<int>(r2.get_cont(), call_data));

                // result = temp1 * temp2;
                new (&__state.result) int(__state._temp1 * __state._temp2);

                if (!r1.done() && !r2.done()) {
                    // yield(result);
                    return prepare_to_suspend(3, get_caller(), __state.result);
        case 3:
                    process_resume(get_caller(), call_data);
                } else {
                    // yield(result);
                    return prepare_to_suspend(_sp_done, get_caller(), __state.result);
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
    printf("*** Test mutiply ***\n");
    range r1(0, 4);
    range r2(2, 10);

    multiply m(r1, r2);

    assert(!m.done());

    while (!m.done()) {
        assert(!r1.done());
        assert(!r2.done());

        int product = m();
        printf("%d\n", product);
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
