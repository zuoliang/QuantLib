/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2008 Simon Ibbotson

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include <ql/experimental/amortizingbonds/amortizingfixedratebond.hpp>
#include <ql/cashflows/cashflowvectors.hpp>
#include <ql/cashflows/simplecashflow.hpp>
#include <ql/time/schedule.hpp>

namespace QuantLib {

    AmortizingFixedRateBond::AmortizingFixedRateBond(
                                      Natural settlementDays,
                                      const std::vector<Real>& notionals,
                                      const Schedule& schedule,
                                      const std::vector<Rate>& coupons,
                                      const DayCounter& accrualDayCounter,
                                      BusinessDayConvention paymentConvention,
                                      const std::vector<Real>& redemptions,
                                      const Date& issueDate)
    : Bond(settlementDays, schedule.calendar(), issueDate),
      frequency_(schedule.tenor().frequency()),
      dayCounter_(accrualDayCounter) {

        maturityDate_ = schedule.endDate();

        cashflows_ = FixedRateLeg(schedule,accrualDayCounter)
            .withNotionals(notionals)
            .withCouponRates(coupons)
            .withPaymentAdjustment(paymentConvention);

        addRedemptionsToCashflows(redemptions);

        QL_ENSURE(!cashflows().empty(), "bond with no cashflows!");
    }

    namespace  {

        Real initialNotional(const Leg& coupons) {
            boost::shared_ptr<FixedRateCoupon> coupon =
                boost::dynamic_pointer_cast<FixedRateCoupon>(coupons.front());

            QL_REQUIRE(coupon,
                       "Coupon input is not a fixed rate coupon");
            return coupon->nominal();
        }

        std::pair<Integer,Integer> daysMinMax(const Period& p) {
            switch (p.units()) {
              case Days:
                return std::make_pair(p.length(), p.length());
              case Weeks:
                return std::make_pair(7*p.length(), 7*p.length());
              case Months:
                return std::make_pair(28*p.length(), 31*p.length());
              case Years:
                return std::make_pair(365*p.length(), 366*p.length());
              default:
                QL_FAIL("unknown time unit (" << Integer(p.units()) << ")");
            }
        }

        bool isSubPeriod(const Period& subPeriod,
                         const Period& superPeriod,
                         Integer& numSubPeriods) {

            std::pair<Integer, Integer> superDays(daysMinMax(superPeriod));
            std::pair<Integer, Integer> subDays(daysMinMax(subPeriod));

            //obtain the approximate time ratio
            Real minPeriodRatio =
                ((Real)superDays.first)/((Real)subDays.second);
            Real maxPeriodRatio =
                ((Real)superDays.second)/((Real)subDays.first);
            Integer lowRatio = static_cast<Integer>(std::floor(minPeriodRatio));
            Integer highRatio = static_cast<Integer>(std::ceil(maxPeriodRatio));

            try {
                for(Integer i=lowRatio; i <= highRatio; ++i) {
                    Period testPeriod = subPeriod * i;
                    if(testPeriod == superPeriod) {
                        numSubPeriods = i;
                        return true;
                    }
                }
            } catch(Error e) {
                return false;
            }

            return false;
        }

        Schedule SinkingSchedule(const Date& startDate,
                                 const Period& maturityTenor,
                                 const Frequency& sinkingFrequency,
                                 const Calendar& paymentCalendar) {
            Period freqPeriod(sinkingFrequency);
            Date maturityDate(startDate + maturityTenor);
            Schedule retVal(startDate, maturityDate, freqPeriod,
                            paymentCalendar, Unadjusted, Unadjusted,
                            DateGeneration::Backward, false);
            return retVal;
        }

        std::vector<Real> SinkingNotionals(const Date& startDate,
                                           const Period& maturityTenor,
                                           const Frequency& sinkingFrequency,
                                           Rate couponRate,
                                           Real initialNotional) {
            Period freqPeriod(sinkingFrequency);
            Integer nPeriods;
            QL_REQUIRE(isSubPeriod(freqPeriod, maturityTenor, nPeriods),
                       "Bond frequency is incompatible with the maturity tenor");

            std::vector<Real> notionals(nPeriods+1);
            notionals.front() = initialNotional;
            Real coupon = couponRate / static_cast<Real>(sinkingFrequency);
            Real compoundedInterest = 1.0;
            Real totalValue = std::pow(1.0+coupon, nPeriods);
            for(Size i = 0; i < (Size)nPeriods-1; ++i) {
                compoundedInterest *= (1.0 + coupon);
                Real currentNotional =
                    initialNotional*(compoundedInterest - (compoundedInterest-1.0)/(1.0 - 1.0/totalValue));
                notionals[i+1] = currentNotional;
            }
            notionals.back() = 0.0;
            return notionals;
        }

        std::vector<Real> SinkingRedemptions(const Date& startDate,
                                             const Period& maturityTenor,
                                             const Frequency& sinkingFrequency,
                                             Rate couponRate,
                                             Real initialNotional) {

            std::vector<Real> notionals =
                SinkingNotionals(startDate, maturityTenor, sinkingFrequency,
                                 couponRate, initialNotional);
            Size nPeriods = notionals.size()-1;
            std::vector<Real> redemptions(nPeriods);

            for(Size i = 0; i < nPeriods; ++i) {
                redemptions[i] =
                    (notionals[i] - notionals[i+1]) / initialNotional * 100;
            }
            return redemptions;
        }

    }


    AmortizingFixedRateBond::AmortizingFixedRateBond(
                                      Natural settlementDays,
                                      const Calendar& calendar,
                                      Real initialFaceAmount,
                                      const Date& startDate,
                                      const Period& bondTenor,
                                      const Frequency& sinkingFrequency,
                                      const Rate coupon,
                                      const DayCounter& accrualDayCounter,
                                      BusinessDayConvention paymentConvention,
                                      const Date& issueDate)
    : Bond(settlementDays, calendar, issueDate),
      frequency_(sinkingFrequency),
      dayCounter_(accrualDayCounter) {

        maturityDate_ = startDate + bondTenor;

        cashflows_ =
            FixedRateLeg(SinkingSchedule(startDate, bondTenor,
                                         sinkingFrequency, calendar),
                         accrualDayCounter)
            .withNotionals(SinkingNotionals(startDate, bondTenor,
                                            sinkingFrequency, coupon,
                                            initialFaceAmount))
            .withCouponRates(coupon)
            .withPaymentAdjustment(paymentConvention);

        addRedemptionsToCashflows();
    }

}