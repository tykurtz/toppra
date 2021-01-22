#ifdef BUILD_WITH_qpOASES
#include <toppra/solver/qpOASES-wrapper.hpp>
#endif
#ifdef BUILD_WITH_GLPK
#include <toppra/solver/glpk-wrapper.hpp>
#endif
#include <toppra/solver/seidel.hpp>

#include <toppra/constraint/linear_joint_acceleration.hpp>
#include <toppra/constraint/linear_joint_velocity.hpp>
#include <toppra/geometric_path.hpp>
#include <toppra/geometric_path/piecewise_poly_path.hpp>

#include "gtest/gtest.h"

constexpr int Ntrials = 10;

std::map<std::string, toppra::Vectors> solutions;

class Solver : public testing::Test {
public:
  Solver() : path(constructPath()), g(Eigen::Vector2d{0.5, 2.}) {

    // path has the equation: 0 * x ^ 3 + 1 * x ^ 2 + 2 x ^ 1 + 3
  }

  std::shared_ptr<toppra::PiecewisePolyPath> constructPath() {
    toppra::Matrix coeff{4, 2};
    coeff.colwise() = toppra::Vector::LinSpaced(4, 0, 3);
    toppra::Matrices coefficents = {coeff, coeff};
    return std::make_shared<toppra::PiecewisePolyPath>(
        coefficents, std::vector<double>{0, 1, 2});
  }

  toppra::Vector getTimes (int N)
  {
    toppra::Bound I (path->pathInterval());
    return toppra::Vector::LinSpaced (N, I[0], I[1]);
  }

  std::shared_ptr<toppra::PiecewisePolyPath> path;
  toppra::Vector g;

  void testSolver(toppra::Solver& solver, const char* name)
  {
    using namespace toppra;

    int nDof = path->dof();
    Vector lb (-Vector::Ones(nDof)),
           ub ( Vector::Ones(nDof));
    LinearConstraintPtr ljv (new constraint::LinearJointVelocity (lb, ub));
    LinearConstraintPtr lja (new constraint::LinearJointAcceleration (lb, ub));

    int N = Ntrials;
    Vector times (getTimes(N));

    solver.initialize ({ ljv, lja }, path, times);

    EXPECT_EQ(solver.nbStages(), N-1);
    EXPECT_EQ(solver.nbVars(), 2);
    ASSERT_EQ(solver.deltas().size(), N-1);
    for (int i = 0; i < N-1; ++i)
      EXPECT_NEAR(solver.deltas()[i], times[i+1] - times[i], 1e-10);

    solver.setupSolver();
    Vector solution;
    Matrix H;
    const value_type infty (std::numeric_limits<value_type>::infinity());
    Bound x, xNext;
    x << -infty, infty;
    xNext << -infty, infty;
    Vectors sols;
    for (int i = 0; i < N; ++i) {
      EXPECT_TRUE(solver.solveStagewiseOptim(i, H, g, x, xNext, solution));
      EXPECT_EQ(solution.size(), solver.nbVars());
      sols.emplace_back(solution);
    }
    solver.closeSolver();

    solutions.emplace(name, sols);
  }
};

#ifdef BUILD_WITH_qpOASES
TEST_F(Solver, qpOASESWrapper) {
  toppra::solver::qpOASESWrapper solver;
  testSolver(solver, "qpOASES");
}
#endif

#ifdef BUILD_WITH_GLPK
TEST_F(Solver, GLPKWrapper) {
  toppra::solver::GLPKWrapper solver;
  testSolver(solver, "GLPK");
}
#endif

TEST_F(Solver, Seidel) {
  toppra::solver::Seidel solver;
  testSolver(solver, "Seidel");
}

// Check that all the solvers returns the same solution.
// TODO each solver is expected to be tested on the same inputs. It should be
// templated, so that we know the same problem is setup (with a template hook to
// enable adding code specific to one solver).
TEST_F(Solver, consistency)
{
  auto ref = solutions.begin();
  bool first = true;
  for(const auto& pair : solutions) {
    if (first) {
      first = false;
      continue;
    }
    ASSERT_EQ(pair.second.size(), ref->second.size());
    for (int i = 0; i < pair.second.size(); ++i) {
      ASSERT_EQ(pair.second[i].size(), ref->second[i].size());
      for (int j = 0; j < pair.second[i].size(); ++j) {
        EXPECT_NEAR(pair.second[i][j], ref->second[i][j], 1e-6)
          << " solvers " << ref->first << " and " << pair.first << " disagree.";
      }
    }
  }
}

#include <toppra/solver/seidel-internal.hpp>

TEST(SeidelFunctions, seidel_1d) {
  using namespace toppra::solver;

  // absolute largest value that a variable can have. This bound should
  // be never be reached, however, in order for the code to work properly.
  constexpr double INF = 1e10;

  RowVector2 v;
  MatrixX2 A (1, 2);

  v = {1, 0};
  {
    /*
     * max   x
     * s.t.  0 - 1    <= 0
     *       x - 1e11 <= 0
     */
    A.resize(2, 2);
    A << 0, -1, // Checks that this constraints is not taken into account.
         1, -INF*10;
    auto sol = seidel::solve_lp1d(v, A);
    EXPECT_TRUE(sol.feasible);
    EXPECT_DOUBLE_EQ(-A(1,1), sol.optvar);
  }

  { // Incoherent bounds.
    /*
     * max   x
     * s.t.  x - 3 <= 0
     *      -x + 4 <= 0
     */
    A.resize(2, 2);
    A << 1, -3,
        -1,  4;
    auto sol = seidel::solve_lp1d(v, A);
    EXPECT_FALSE(sol.feasible);
  }

  { // Incoherent bounds.
    /*
     * max   x
     * s.t.  x + inf <= 0
     */
    A.resize(1, 2);
    A << 1, seidel::infinity;
    auto sol = seidel::solve_lp1d(v, A);
    EXPECT_FALSE(sol.feasible);
  }

  v = {-1, 0};
  {
    /*
     * max   -x
     * s.t.   x - 1e11 <= 0
     */
    A.resize(1, 2);
    A << 1, -INF*10;
    auto sol = seidel::solve_lp1d(v, A);
    EXPECT_TRUE(sol.feasible);
    EXPECT_DOUBLE_EQ(-seidel::infinity, sol.optvar);
  }

  {
    /*
     * max   -x
     * s.t.  -x + 3 <= 0
     */
    A.resize(1, 2);
    A << -1, 3;
    auto sol = seidel::solve_lp1d(v, A);
    EXPECT_TRUE(sol.feasible);
    EXPECT_DOUBLE_EQ(3, sol.optvar);
  }

  v = {0, 0};
  {
    /*
     * max   0.
     * s.t.  -x + 3 <= 0
     */
    A.resize(1, 2);
    A << -1, 3;
    auto sol = seidel::solve_lp1d(v, A);
    EXPECT_TRUE(sol.feasible);
    EXPECT_DOUBLE_EQ(3, sol.optvar);
  }

  {
    /*
     * max   0.
     * s.t.  x - 1 <= 0
     *      -x - 1 <= 0
     */
    A.resize(2, 2);
    A <<  1, -1,
         -1, -1;
    auto sol = seidel::solve_lp1d(v, A);
    EXPECT_TRUE(sol.feasible);
    EXPECT_DOUBLE_EQ(0, sol.optvar);
  }

  v = {seidel::TINY/2, 0};
  {
    /*
     * max   TINY * x
     * s.t.  x - 1 <= 0
     *      -x - 1 <= 0
     */
    A.resize(2, 2);
    A <<  1, -1,
         -1, -1;
    auto sol = seidel::solve_lp1d(v, A);
    EXPECT_TRUE(sol.feasible);
    EXPECT_DOUBLE_EQ(1, sol.optvar);
  }
}

TEST(SeidelFunctions, seidel_2d) {
  using namespace toppra::solver;
  RowVector2 v;
  MatrixX3 A;
  Vector2 low, high;
  std::array<int, 2> active_c {0, 0};
  std::vector<int> index_map;
  MatrixX2 A_1d;
  seidel::LpSol sol;

  using Eigen::VectorXd;

  auto check = [&A, &low, &high, &sol](bool expectFeasible){
    auto LooseNegative = [](const Eigen::VectorXd &a) { return (a.array() < seidel::TINY).all(); };
    auto CompareDouble = [](double a, double b) { return (a == b || fabs(a-b) < seidel::TINY); };
    auto values = [](const MatrixX3& coeffs, const Vector2& vars) -> VectorXd {
      VectorXd res (coeffs.rightCols<1>());
      for (int i = 0; i < 2; ++i) {
        if (std::isfinite(vars[i]))
          res += coeffs.col(i)*vars[i];
        else
          res += (coeffs.col(i).array() == 0.).select(0., coeffs.col(i)*vars[i]);
      }
      return res;
    };

    EXPECT_EQ(expectFeasible, sol.feasible);
    if (sol.feasible) {
      Eigen::VectorXd A_times_optvar = values(A, sol.optvar);

      // Check that all constraints are statisfied.
      EXPECT_PRED1(LooseNegative, A_times_optvar);
      // Check that active constraints are approximatevely zero.
      for (int i = 0; i < 2; ++i) {
        ASSERT_LT(sol.active_c[i], A.rows());
        ASSERT_GE(sol.active_c[i], -4);
        if (sol.active_c[i] < 0) {
          switch (sol.active_c[i]) {
            case seidel::LOW_0 : EXPECT_PRED2(CompareDouble, low [0], sol.optvar[0]); break;
            case seidel::HIGH_0: EXPECT_PRED2(CompareDouble, high[0], sol.optvar[0]); break;
            case seidel::LOW_1 : EXPECT_PRED2(CompareDouble, low [1], sol.optvar[1]); break;
            case seidel::HIGH_1: EXPECT_PRED2(CompareDouble, high[1], sol.optvar[1]); break;
          }
        } else {
          EXPECT_NEAR(A_times_optvar[sol.active_c[i]], 0., seidel::TINY);
        }
      }
    }
  };

  {
    A.resize(16, 3);
    A_1d.resize(A.rows()+4, 2);
    v << 0.04, 1;
    A <<
           -0.04,           -1,           0,
            0.04,            1,    -2.26492,
       -0.087295,    0.0654839,    -28.3501,
        0.258242,    -0.114201,    -43.4429,
      -0.0964134,     0.262025,    -27.1247,
        0.117863,    -0.191702,    -27.7368,
      0.00258571,   0.00680004,    -14.5918,
        0.017961,   -0.0688431,    -14.1562,
    -0.000553256,   0.00212078,    -14.7322,
        0.087295,   -0.0654839,    -28.3499,
       -0.258242,     0.114201,    -13.2571,
       0.0964134,    -0.262025,    -29.5753,
       -0.117863,     0.191702,    -28.9632,
     -0.00258571,  -0.00680004,    -14.8082,
       -0.017961,    0.0688431,    -15.2438,
     0.000553256,  -0.00212078,    -14.6678;

    low  << -seidel::infinity, 2.06944;
    high <<  seidel::infinity, 2.06944 - 2.22045e-14;

    sol = seidel::solve_lp2d(v, A, low, high,
        active_c, false, index_map, A_1d);

    check(true);
  }

  {
    A.resize(1, 3);
    A_1d.resize(A.rows()+4, 2);
    v = { -1, 1};
    A << 0, 1, -2;
    low = { -seidel::infinity, 0};
    high = { seidel::infinity, 1};

    sol = seidel::solve_lp2d(v, A, low, high,
        active_c, false, index_map, A_1d);

    check(true);
  }

  {
    A.resize(17, 3);
    A_1d.resize(A.rows()+4, 2);
    v = { -1e-09, 1};
    A <<
           -0.04,           -1,            0,
            0.04,            1,     -96.6645,
      0.00950227,    0.0127432,     -28.3499,
     -0.00102272,   -0.0430245,     -15.0205,
     -0.00468815,   -0.0189229,     -27.9265,
     0.000181267,    0.0116963,     -29.9426,
     0.000228502,   0.00118769,     -14.7433,
    -0.000139133,    -0.000333,     -14.6901,
    -4.48311e-05, -0.000264438,     -14.6928,
     -0.00950227,   -0.0127432,     -28.3501,
      0.00102272,    0.0430245,     -41.6795,
      0.00468815,    0.0189229,     -28.7735,
    -0.000181267,   -0.0116963,     -26.7574,
    -0.000228502,  -0.00118769,     -14.6567,
     0.000139133,     0.000333,     -14.7099,
     4.48311e-05,  0.000264438,     -14.7072,
               0,  0.000174979,      -0.0225;
    low = { -seidel::infinity, 0};
    high = { seidel::infinity, 100};
    active_c = {0, 16};

    index_map.resize(17);
    for (int i = 0; i < index_map.size(); ++i) index_map[i] = i;
    sol = seidel::solve_lp2d(v, A, low, high,
        active_c, true, index_map, A_1d);

    check(true);
  }

  {
    A.resize(17, 3);
    A_1d.resize(A.rows()+4, 2);
    v = { 1e-09, 1};
    A <<
           -0.04,           -1,      99.8403,
            0.04,            1,         -100,
      -0.0020703,  0.000263966,     -28.3499,
      0.00442128, -0.000610605,     -12.4686,
     0.000369489, -4.18819e-05,     -30.4098,
      -0.0011122,  0.000181629,     -28.8316,
     8.41287e-05, -1.23018e-05,     -14.8322,
    -0.000242411,   3.7092e-05,     -15.3541,
     4.13197e-06, -6.85238e-07,     -14.7261,
       0.0020703, -0.000263966,     -28.3501,
     -0.00442128,  0.000610605,     -44.2314,
    -0.000369489,  4.18819e-05,     -26.2902,
       0.0011122, -0.000181629,     -27.8684,
    -8.41287e-05,  1.23018e-05,     -14.5678,
     0.000242411,  -3.7092e-05,     -14.0459,
    -4.13197e-06,  6.85238e-07,     -14.6739,
               0,  8.08395e-06,      -0.0225;
    low = { -seidel::infinity, 0};
    high = { seidel::infinity, 100};

    sol = seidel::solve_lp2d(v, A, low, high,
        active_c, false, index_map, A_1d);

    check(true);
  }
}
