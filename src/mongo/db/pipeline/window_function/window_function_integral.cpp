/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/pipeline/window_function/window_function_integral.h"
#include "mongo/db/pipeline/accumulator.h"

namespace mongo {

Value WindowFunctionIntegral::integralOfTwoPointsByTrapezoidalRule(const Value& preValue,
                                                                   const Value& newValue) {
    auto preArr = preValue.getArray();
    auto newArr = newValue.getArray();

    if (preArr[0].isNaN() || preArr[1].isNaN() || newArr[0].isNaN() || newArr[1].isNaN())
        return Value(0);


    if ((preArr[0].getType() == BSONType::Date && newArr[0].getType() == BSONType::Date) ||
        (preArr[0].numeric() && newArr[0].numeric())) {
        // Now 'newValue' and 'preValue' are either both numeric, or both dates.
        // $subtract on two dates gives us the difference in milliseconds.
        Value delta = uassertStatusOK(ExpressionSubtract::apply(newArr[0], preArr[0]));
        Value sumY = uassertStatusOK(ExpressionAdd::apply(newArr[1], preArr[1]));
        Value integral = uassertStatusOK(ExpressionMultiply::apply(sumY, delta));

        return uassertStatusOK(ExpressionDivide::apply(integral, Value(2.0)));
    } else {
        return Value(0);
    }
}

void WindowFunctionIntegral::assertValueType(const Value& value) {
    tassert(5423900,
            "The input value of $integral window function must be a vector of 2 value, the first "
            "value must be numeric or date type and the second must be numeric.",
            value.isArray() && value.getArray().size() == 2 && value.getArray()[1].numeric() &&
                (value.getArray()[0].numeric() || value.getArray()[0].getType() == BSONType::Date));

    auto arr = value.getArray();
    if (_outputUnitMillis) {
        tassert(5423901,
                "$integral with 'outputUnit' expects the sortBy field to be a Date",
                arr[0].getType() == BSONType::Date);
    } else {
        tassert(5423902,
                "$integral (with no 'outputUnit') expects the sortBy field to be numeric",
                arr[0].numeric());
    }
}

void WindowFunctionIntegral::add(Value value) {
    assertValueType(value);

    auto arr = value.getArray();
    if (arr[0].isNaN() || arr[1].isNaN())
        _nanCount++;

    // Update "_integral" if there are at least two values including the value to add.
    if (_values.size() > 0) {
        _integral.add(integralOfTwoPointsByTrapezoidalRule(_values.back(), value));
    }

    _memUsageBytes += value.getApproximateSize();
    _values.emplace_back(std::move(value));
}

void WindowFunctionIntegral::remove(Value value) {
    assertValueType(value);
    tassert(5423903, "Can't remove from an empty WindowFunctionIntegral", _values.size() > 0);
    tassert(
        5423904,
        "Attempted to remove an element other than the first element from WindowFunctionIntegral",
        _expCtx->getValueComparator().evaluate(_values.front() == value));

    auto arr = value.getArray();
    if (arr[0].isNaN() || arr[1].isNaN())
        _nanCount--;

    _memUsageBytes -= value.getApproximateSize();
    _values.pop_front();

    // Update "_integral" if there are at least two values before removing the current value.
    // In the case that the value to remove is the last value in the window, the integral is
    // guaranteed to be 0, so there is no need to update '_integral'.
    if (_values.size() > 0) {
        _integral.remove(integralOfTwoPointsByTrapezoidalRule(value, _values.front()));
    }
}

}  // namespace mongo
