/*
 * Copyright (c) 2018-2019,2021 LAAS/CNRS
 * All rights reserved.
 *
 * Redistribution  and  use  in  source  and binary  forms,  with  or  without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of  source  code must retain the  above copyright
 *      notice and this list of conditions.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice and  this list of  conditions in the  documentation and/or
 *      other materials provided with the distribution.
 *
 * THE SOFTWARE  IS PROVIDED "AS IS"  AND THE AUTHOR  DISCLAIMS ALL WARRANTIES
 * WITH  REGARD   TO  THIS  SOFTWARE  INCLUDING  ALL   IMPLIED  WARRANTIES  OF
 * MERCHANTABILITY AND  FITNESS.  IN NO EVENT  SHALL THE AUTHOR  BE LIABLE FOR
 * ANY  SPECIAL, DIRECT,  INDIRECT, OR  CONSEQUENTIAL DAMAGES  OR  ANY DAMAGES
 * WHATSOEVER  RESULTING FROM  LOSS OF  USE, DATA  OR PROFITS,  WHETHER  IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR  OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *                                           Anthony Mallet on Fri Jun  1 2018
 */
#include "acuavpos.h"

#include <sys/time.h>
#include <aio.h>
#include <err.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Eigen/Core"
#include "Eigen/Dense"

#include "codels.h"

/*
 * --- uavpos_controller ---------------------------------------------------
 *
 * Implements the position bits of the controller described in:
 *
 * T. Lee, M. Leoky and N. H. McClamroch, "Geometric tracking control of a
 * quadrotor UAV on SE(3)", 49th IEEE Conference on Decision and Control
 * (CDC), Atlanta, GA, 2010, pp. 5420-5425.
 */

int
uavpos_controller(const uavpos_ids_body_s *body,
                  const uavpos_ids_servo_s *servo,
                  const or_pose_estimator_state *state,
                  const or_rigid_body_state *desired,
                  uavpos_log_s *log,
                  or_uav_input *uav_input)
{
  using namespace Eigen;

  Matrix3d Rd;
  Quaternion<double> qd;
  Vector3d xd, vd, wd, ad, awd;

  Vector3d x, v;

  Vector3d ex, ev;
  static Vector3d Iex;

  Vector3d f;
  Vector3d b3, b3d, k, k1, k2;
  double thrust, theta, theta_max, theta_min, c1, c2;

  int i;

  static bool emerg_x, emerg_v;

  /* gains */
  const Array3d Kp(servo->gain.Kpxy, servo->gain.Kpxy, servo->gain.Kpz);
  const Array3d Ki(servo->gain.Kixy, servo->gain.Kixy, servo->gain.Kiz);
  const Array3d Kv(servo->gain.Kvxy, servo->gain.Kvxy, servo->gain.Kvz);


  /* desired state */
  if (desired->pos._present) {
    xd <<
      desired->pos._value.x, desired->pos._value.y, desired->pos._value.z;
  } else {
    xd << 0., 0., 0.;
    Iex << 0., 0., 0.;
  }
  if (desired->att._present) {
    qd.coeffs() <<
      desired->att._value.qx, desired->att._value.qy, desired->att._value.qz,
      desired->att._value.qw;
  } else {
    qd = Quaternion<double>::Identity();
  }

  if (desired->vel._present) {
    vd <<
      desired->vel._value.vx, desired->vel._value.vy, desired->vel._value.vz;
  } else {
    vd << 0., 0., 0.;
  }
  if (desired->avel._present) {
    wd <<
      desired->avel._value.wx, desired->avel._value.wy, desired->avel._value.wz;
  } else {
    wd << 0., 0., 0.;
  }

  if (desired->acc._present) {
    ad <<
      desired->acc._value.ax,
      desired->acc._value.ay,
      desired->acc._value.az;
  } else {
    ad << 0., 0., 0.;
  }

  if (desired->aacc._present) {
    awd <<
      desired->aacc._value.awx,
      desired->aacc._value.awy,
      desired->aacc._value.awz;
  } else {
    awd << 0., 0., 0.;
  }


  /* current state */
  if (state->pos._present && !std::isnan(state->pos._value.x) &&
      state->pos_cov._present &&
      state->pos_cov._value.cov[0] < servo->emerg.dx &&
      state->pos_cov._value.cov[2] < servo->emerg.dx &&
      state->pos_cov._value.cov[5] < servo->emerg.dx) {
    x << state->pos._value.x, state->pos._value.y, state->pos._value.z;
    if (!desired->pos._present)
      xd = x + Eigen::Vector3d(0, 0, -5e-2);

    if (emerg_x)
      warnx("recovered accurate position estimation");
    emerg_x = false;
  } else {
    if (!emerg_x)
      warnx(
        "emergency: inaccurate position estimation (stddev %g)",
        state->pos._present ?
        std::sqrt(std::max(
                    std::max(
                      state->pos_cov._value.cov[0],
                      state->pos_cov._value.cov[2]),
                    state->pos_cov._value.cov[5])) :
        nan(""));
    emerg_x = true;

    x = xd;
    Iex << 0., 0., 0.;
    ad = Eigen::Vector3d(0, 0, - servo->emerg.descent);
  }

  if (state->vel._present && !std::isnan(state->vel._value.vx) &&
      state->vel_cov._present &&
      state->vel_cov._value.cov[0] < servo->emerg.dv &&
      state->vel_cov._value.cov[2] < servo->emerg.dv &&
      state->vel_cov._value.cov[5] < servo->emerg.dv) {
    v << state->vel._value.vx, state->vel._value.vy, state->vel._value.vz;

    if (emerg_v)
      warnx("recovered accurate velocity estimation");
    emerg_v = false;
  } else {
    if (!emerg_v)
      warnx(
        "emergency: inaccurate velocity estimation (stddev %g)",
        state->vel._present ?
        std::sqrt(std::max(
                    std::max(
                      state->vel_cov._value.cov[0],
                      state->vel_cov._value.cov[2]),
                    state->vel_cov._value.cov[5])) :
        nan(""));
    emerg_v = true;

    v = vd;
    ad = Eigen::Vector3d(0, 0, - servo->emerg.descent);
  }


  /* position error */
  ex = x - xd;
  for(i = 0; i < 3; i++)
    if (fabs(ex(i)) > servo->sat.x) ex(i) = copysign(servo->sat.x, ex(i));

  Iex += ex * uavpos_control_period_ms/1000.;
  for(i = 0; i < 3; i++)
    if (fabs(Iex(i)) > servo->sat.ix) Iex(i) = copysign(servo->sat.ix, Iex(i));


  /* velocity error */
  ev = v - vd;
  for(i = 0; i < 3; i++)
    if (fabs(ev(i)) > servo->sat.v) ev(i) = copysign(servo->sat.v, ev(i));


  /* desired thrust */
  f =
    - Kp * ex.array() - Kv * ev.array() - Ki * Iex.array()
    + body->mass * (Eigen::Vector3d(0, 0, 9.81) + ad).array();


  /* desired orientation */
  if (body->rxy < 1e-3)
    b3d = f.normalized(); /* trivial solution without full actuation */
  else {
    b3d = qd.matrix().col(2).normalized();

    k = b3d.cross(f);
    c1 = k.norm();
    if (c1 > 1e-3 /* f not parallel to b3d */) {
      k /= c1;
      k1 = k.cross(b3d);
      k2 = k * k.dot(b3d);

      thrust = f.norm();
      if (thrust > body->rxy) {
        c2 = std::sqrt(thrust * thrust - body->rxy * body->rxy);
        theta_max = std::asin(c1 / thrust);
        theta_min = 0.;

        /* optimize until the thrust is acceptable */
        b3 = b3d;
        while(theta_max - theta_min > 1e-3) {
          theta = (theta_min + theta_max) / 2.;
          b3d = b3 * std::cos(theta) +
                k1 * std::sin(theta) + k2 * (1. - std::cos(theta));

          (f.dot(b3d) >= c2 ? theta_max : theta_min) = theta;
        }
      }
    }
  }

  Rd.col(2) = b3d;
  Rd.col(1) = Rd.col(2).cross(qd.matrix().col(0)).normalized();
  Rd.col(0) = Rd.col(1).cross(Rd.col(2));

  qd = Quaternion<double>(Rd);


  /* limit thrust in the XY plane */
  k = Rd.transpose() * f;
  c1 = k.head<2>().norm();
  if (c1 > body->rxy) {
    k.head<2>() *= body->rxy / c1;
    f = Rd * k;
  }


  /* output */
  uav_input->intrinsic = false;

  uav_input->thrust._present = true;
  uav_input->thrust._value.x = f(0);
  uav_input->thrust._value.y = f(1);
  uav_input->thrust._value.z = f(2);

  uav_input->att._present = true;
  uav_input->att._value.qw = qd.w();
  uav_input->att._value.qx = qd.vec()(0);
  uav_input->att._value.qy = qd.vec()(1);
  uav_input->att._value.qz = qd.vec()(2);

  uav_input->avel._present = true;
  uav_input->avel._value.wx = wd(0);
  uav_input->avel._value.wy = wd(1);
  uav_input->avel._value.wz = wd(2);

  uav_input->aacc._present = true;
  uav_input->aacc._value.awx = awd(0);
  uav_input->aacc._value.awy = awd(1);
  uav_input->aacc._value.awz = awd(2);


  /* logging */
  if (log->req.aio_fildes >= 0) {
    log->total++;
    if (log->total % log->decimation == 0) {
      if (log->pending) {
        if (aio_error(&log->req) != EINPROGRESS) {
          log->pending = false;
          if (aio_return(&log->req) <= 0) {
            warn("log");
            close(log->req.aio_fildes);
            log->req.aio_fildes = -1;
          }
        } else {
          log->skipped = true;
          log->missed++;
        }
      }
    }

    if (log->req.aio_fildes >= 0 && !log->pending) {
      double d;
      double roll, pitch, yaw;

      d = hypot(Rd(0,0), Rd(1,0));
      if (fabs(d) > 1e-10) {
        yaw = atan2(Rd(1,0), Rd(0,0));
        roll = atan2(Rd(2,1), Rd(2,2));
      } else {
        yaw = atan2(-Rd(0,1), Rd(1,1));
        roll = 0.;
      }
      pitch = atan2(-Rd(2,0), d);

      log->req.aio_nbytes = snprintf(
        log->buffer, sizeof(log->buffer),
        "%s" uavpos_log_fmt "\n",
        log->skipped ? "\n" : "",
        uav_input->ts.sec, uav_input->ts.nsec,
        uav_input->ts.sec - state->ts.sec +
        (uav_input->ts.nsec - state->ts.nsec)*1e-9,
        f(0), f(1), f(2),
        xd(0), xd(1), xd(2), roll, pitch, yaw,
        vd(0), vd(1), vd(2), wd(0), wd(1), wd(2),
        ad(0), ad(1), ad(2), awd(0), awd(1), awd(2),
        ex(0), ex(1), ex(2), ev(0), ev(1), ev(2));

      if (aio_write(&log->req)) {
        warn("log");
        close(log->req.aio_fildes);
        log->req.aio_fildes = -1;
      } else
        log->pending = true;

      log->skipped = false;
    }
  }

  return 0;
}
