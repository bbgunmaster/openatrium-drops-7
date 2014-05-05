<?php
/**
 * @file
 * Utility functions for Date iCal. Many of these are re-writes of buggy Date
 * module code.
 */

/**
 * Parse the repeat data into date values.
 *
 * This is a re-write of date_repeat_build_dates() which fixes it's bugs
 * regarding multi-property RDATEs and EXDATEs.
 */
function _date_ical_get_repeat_dates($field_name, $repeat_data, $item, $source) {
  module_load_include('inc', 'date_api', 'date_api_ical');
  $field_info = field_info_field($field_name);
  
  $rrule_values = _date_ical_parse_repeat_rule($repeat_data['RRULE']);
  //$exrule_values = _date_ical_parse_repeat_rule($repeat_data['EXRULE']);
  $rdates = _date_ical_parse_repeat_dates($repeat_data['RDATE']);
  $exdates = _date_ical_parse_repeat_dates($repeat_data['EXDATE']);
  
  // By the time we get here, the start and end dates have been
  // adjusted back to UTC, but we want localtime dates to do
  // things like '+1 Tuesday', so adjust back to localtime.
  $timezone = date_get_timezone($field_info['settings']['tz_handling'], $item['timezone']);
  $timezone_db = date_get_timezone_db($field_info['settings']['tz_handling']);
  $start = new DateObject($item['value'], $timezone_db, date_type_format($field_info['type']));
  $start->limitGranularity($field_info['settings']['granularity']);
  if ($timezone != $timezone_db) {
    date_timezone_set($start, timezone_open($timezone));
  }
  if (!empty($item['value2']) && $item['value2'] != $item['value']) {
    $end = new DateObject($item['value2'], date_get_timezone_db($field_info['settings']['tz_handling']), date_type_format($field_info['type']));
    $end->limitGranularity($field_info['settings']['granularity']);
    date_timezone_set($end, timezone_open($timezone));
  }
  else {
    $end = $start;
  }
  $duration = $start->difference($end);
  $start_datetime = date_format($start, DATE_FORMAT_DATETIME);
  
  if (!empty($rrule_values['UNTIL']['datetime'])) {
    // TODO: The spec says that UNTIL must be specified in UTC, but I think
    // this may not be followd by all feed creators. It may be a good idea to
    // support optionally assuming the date has the same TZID as DTSTART.
    $end = date_ical_date($rrule_values['UNTIL'], $timezone);
    $end_datetime = date_format($end, DATE_FORMAT_DATETIME);
  }
  elseif (!empty($rrule_values['COUNT'])) {
    $end_datetime = NULL;
  }
  else {
    // No UNTIL and no COUNT?
    return array();
  }
  
  // Convert the EXDATE and RDATE values to datetime strings.
  // Even though exdates and rdates can be specified to the second, Date
  // Repeat's code checks them by comparing them to the date value only.
  $exceptions = array();
  foreach ($exdates as $exception) {
    $date = date_ical_date($exception, $timezone);
    $exceptions[] = date_format($date, 'Y-m-d');
  }
  $additions = array();
  foreach ($rdates as $rdate) {
    $date = date_ical_date($rdate, $timezone);
    $additions[] = date_format($date, 'Y-m-d');
  }
  // TODO: EXRULE.
  
  $date_repeat_compatible_rrule = "{$repeat_data['RRULE']}\n{$repeat_data['RDATE']}\n{$repeat_data['EXDATE']}";
  $calculated_dates = date_repeat_calc($date_repeat_compatible_rrule, $start_datetime, $end_datetime, $exceptions, $timezone, $additions);
  $repeat_dates = array();
  foreach ($calculated_dates as $delta => $date) {
    // date_repeat_calc always returns DATE_DATETIME dates, which is
    // not necessarily $field_info['type'] dates.
    // Convert returned dates back to db timezone before storing.
    $date_start = new DateObject($date, $timezone, DATE_FORMAT_DATETIME);
    $date_start->limitGranularity($field_info['settings']['granularity']);
    date_timezone_set($date_start, timezone_open($timezone_db));
    $date_end = clone($date_start);
    date_modify($date_end, '+' . $duration . ' seconds');
    $repeat_dates[$delta] = array(
      'value' => date_format($date_start, date_type_format($field_info['type'])),
      'value2' => date_format($date_end, date_type_format($field_info['type'])),
      'offset' => date_offset_get($date_start),
      'offset2' => date_offset_get($date_end),
      'timezone' => $timezone,
      'rrule' => $date_repeat_compatible_rrule,
    );
  }
  return $repeat_dates;
}

/**
 * Parse an rrule or exrule string.
 *
 * @return array
 *   Array in the form of PROPERTY => array(VALUES)
 *   PROPERTIES include FREQ, INTERVAL, COUNT, BYDAY, BYMONTH, BYYEAR, UNTIL.
 */
function _date_ical_parse_repeat_rule($repeat_rule_string) {
  module_load_include('inc', 'date_api', 'date_api_ical');
  
  $repeat_rule_string = preg_replace('/(R|EX)RULE.*:/', '', $repeat_rule_string);
  $items = array('DATA' => $repeat_rule_string);
  foreach (explode(';', $repeat_rule_string) as $recur_val) {
    list($key, $value) = explode('=', $recur_val);
    // Must be some kind of invalid data.
    if (empty($key) || empty($value)) {
      continue;
    }
    
    // These keys never have multiple values.
    if (in_array($key, array('UNTIL', 'FREQ', 'INTERVAL', 'COUNT', 'WKST'))) {
      if ($key == 'UNTIL') {
        // This is a function from the date_api module, not date_ical.
        $value = date_ical_parse_date('', $value);
      }
    }
    else {
      // The rest can be multi-value csv strings.
      $value = explode(',', $value);
    }
    
    $items[$key] = $value;
  }
  return $items;
}

/**
 * Parses an iCal RDATE or EXDATE, including multi-property rules.
 *
 * @param string $repeat_date_string
 *   An RDATE or EXDATE property string, e.g.
 *   "EXDATE;TZID=America/Los_Angeles:20130415T180000,20130422T180000"
 *   Can be multiple EXDATE or RDATE properties, separated by newlines.
 *
 * @return array
 *   An array of dates returned by date_ical_parse_date().
 */
function _date_ical_parse_repeat_dates($repeat_date_string) {
  module_load_include('inc', 'date_api', 'date_api_ical');
  
  $properties = explode("\n", str_replace("\r\n", "\n", $repeat_date_string));
  $parsed_dates = array();
  
  foreach ($properties as $property) {
    $matches = array();
    if (preg_match('/(R|EX)DATE([^:]*):(.*)/', $property, $matches)) {
      $params = $matches[2];
      foreach (explode(',', $matches[3]) as $date) {
        $parsed_dates[] = date_ical_parse_date($params, $date);
      }
    }
  }
  return $parsed_dates;
}