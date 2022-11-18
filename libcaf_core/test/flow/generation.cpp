// This file is part of CAF, the C++ Actor Framework. See the file LICENSE in
// the main distribution directory for license terms and copyright or visit
// https://github.com/actor-framework/actor-framework/blob/master/LICENSE.

#define CAF_SUITE flow.generation

#include "caf/flow/observable.hpp"

#include "core-test.hpp"

#include "caf/flow/coordinator.hpp"
#include "caf/flow/merge.hpp"
#include "caf/flow/observable_builder.hpp"
#include "caf/flow/observer.hpp"
#include "caf/flow/scoped_coordinator.hpp"

using namespace caf;

namespace {

using ivec = std::vector<int>;

struct fixture : test_coordinator_fixture<> {
  flow::scoped_coordinator_ptr ctx = flow::make_scoped_coordinator();
};

auto iota_vec(size_t n, int init = 1) {
  auto result = ivec{};
  result.resize(n);
  std::iota(result.begin(), result.end(), init);
  return result;
}

} // namespace

BEGIN_FIXTURE_SCOPE(fixture)

SCENARIO("repeater sources repeat one value indefinitely") {
  GIVEN("a repeater source") {
    WHEN("subscribing to its output") {
      THEN("the observer receives the same value over and over again") {
        auto snk = flow::make_passive_observer<int>();
        ctx->make_observable().repeat(42).subscribe(snk->as_observer());
        CHECK_EQ(snk->state, flow::observer_state::subscribed);
        CHECK(snk->buf.empty());
        if (CHECK(snk->sub)) {
          snk->sub.request(3);
          ctx->run();
          CHECK_EQ(snk->buf, ivec({42, 42, 42}));
          snk->sub.request(4);
          ctx->run();
          CHECK_EQ(snk->buf, ivec({42, 42, 42, 42, 42, 42, 42}));
          snk->sub.dispose();
          ctx->run();
          CHECK_EQ(snk->buf, ivec({42, 42, 42, 42, 42, 42, 42}));
        }
      }
    }
  }
}

SCENARIO("container sources stream their input values") {
  GIVEN("a container source") {
    WHEN("subscribing to its output") {
      THEN("the observer receives the values from the container in order") {
        auto xs = ivec{1, 2, 3, 4, 5, 6, 7};
        auto snk = flow::make_passive_observer<int>();
        ctx->make_observable()
          .from_container(std::move(xs))
          .subscribe(snk->as_observer());
        CHECK_EQ(snk->state, flow::observer_state::subscribed);
        CHECK(snk->buf.empty());
        if (CHECK(snk->sub)) {
          snk->sub.request(3);
          ctx->run();
          CHECK_EQ(snk->buf, ivec({1, 2, 3}));
          snk->sub.request(21);
          ctx->run();
          CHECK_EQ(snk->buf, ivec({1, 2, 3, 4, 5, 6, 7}));
          CHECK_EQ(snk->state, flow::observer_state::completed);
        }
      }
    }
    WHEN("combining it with with a step that limits the amount of items") {
      THEN("the observer receives the defined subset of values") {
        auto xs = iota_vec(713);
        auto snk = flow::make_passive_observer<int>();
        auto res = ivec{};
        ctx->make_observable()
          .from_container(std::move(xs))
          .take(678)
          .for_each([&res](int val) { res.push_back(val); });
        ctx->run();
        CHECK_EQ(res, iota_vec(678));
      }
    }
  }
}

SCENARIO("value sources produce exactly one input") {
  GIVEN("a value source") {
    WHEN("subscribing to its output") {
      THEN("the observer receives one value") {
        auto snk = flow::make_passive_observer<int>();
        ctx->make_observable().just(42).subscribe(snk->as_observer());
        CHECK_EQ(snk->state, flow::observer_state::subscribed);
        CHECK(snk->buf.empty());
        if (CHECK(snk->sub)) {
          snk->sub.request(100);
          ctx->run();
          CHECK_EQ(snk->buf, ivec({42}));
          CHECK_EQ(snk->state, flow::observer_state::completed);
        }
      }
    }
  }
}

SCENARIO("callable sources stream values generated from a function object") {
  GIVEN("a callable source returning non-optional values") {
    WHEN("subscribing to its output") {
      THEN("the observer receives an indefinite amount of values") {
        auto f = [n = 1]() mutable { return n++; };
        auto snk = flow::make_passive_observer<int>();
        ctx->make_observable().from_callable(f).subscribe(snk->as_observer());
        CHECK_EQ(snk->state, flow::observer_state::subscribed);
        CHECK(snk->buf.empty());
        if (CHECK(snk->sub)) {
          snk->sub.request(3);
          ctx->run();
          CHECK_EQ(snk->buf, ivec({1, 2, 3}));
          snk->sub.request(4);
          ctx->run();
          CHECK_EQ(snk->buf, ivec({1, 2, 3, 4, 5, 6, 7}));
          snk->sub.dispose();
          ctx->run();
          CHECK_EQ(snk->buf, ivec({1, 2, 3, 4, 5, 6, 7}));
        }
      }
    }
    WHEN("combining it with with a step that accepts a finite amount") {
      THEN("the observer receives a fixed amount of values") {
        auto res = ivec{};
        auto f = [n = 1]() mutable { return n++; };
        ctx
          ->make_observable() //
          .from_callable(f)
          .take(713)
          .for_each([&res](int val) { res.push_back(val); });
        ctx->run();
        CHECK_EQ(res, iota_vec(713));
      }
    }
  }
  GIVEN("a callable source returning optional values") {
    WHEN("subscribing to its output") {
      THEN("the observer receives value until the callable return null-opt") {
        auto f = [n = 1]() mutable -> std::optional<int> {
          if (n < 8)
            return n++;
          else
            return std::nullopt;
        };
        auto snk = flow::make_passive_observer<int>();
        ctx->make_observable().from_callable(f).subscribe(snk->as_observer());
        CHECK_EQ(snk->state, flow::observer_state::subscribed);
        CHECK(snk->buf.empty());
        if (CHECK(snk->sub)) {
          snk->sub.request(3);
          ctx->run();
          CHECK_EQ(snk->buf, ivec({1, 2, 3}));
          snk->sub.request(21);
          ctx->run();
          CHECK_EQ(snk->buf, ivec({1, 2, 3, 4, 5, 6, 7}));
          CHECK_EQ(snk->state, flow::observer_state::completed);
        }
      }
    }
    WHEN("combining it with with a step that accepts a finite amount") {
      THEN("the observer receives a fixed amount of values") {
        auto res = ivec{};
        auto f = [n = 1]() mutable -> std::optional<int> { return n++; };
        ctx
          ->make_observable() //
          .from_callable(f)
          .take(713)
          .for_each([&res](int val) { res.push_back(val); });
        ctx->run();
        CHECK_EQ(res, iota_vec(713));
      }
    }
  }
}

namespace {

class custom_generator {
public:
  using output_type = int;

  template <class Step, class... Steps>
  void pull(size_t n, Step& step, Steps&... steps) {
    for (size_t i = 0; i < n; ++i) {
      if (value_ > 7) {
        step.on_complete(steps...);
        return;
      } else if (!step.on_next(value_++, steps...)) {
        return;
      }
    }
  }

private:
  int value_ = 1;
};

} // namespace

SCENARIO("lifting converts a generator into an observable") {
  GIVEN("a lifted implementation of the generator concept") {
    WHEN("subscribing to its output") {
      THEN("the observer receives the generated values") {
        auto snk = flow::make_passive_observer<int>();
        auto f = custom_generator{};
        ctx->make_observable().from_generator(f).subscribe(snk->as_observer());
        CHECK_EQ(snk->state, flow::observer_state::subscribed);
        CHECK(snk->buf.empty());
        CHECK(snk->subscribed());
        snk->request(3);
        ctx->run();
        CHECK_EQ(snk->buf, ivec({1, 2, 3}));
        snk->sub.request(21);
        ctx->run();
        CHECK_EQ(snk->buf, ivec({1, 2, 3, 4, 5, 6, 7}));
        CHECK(snk->completed());
      }
    }
  }
}

END_FIXTURE_SCOPE()
