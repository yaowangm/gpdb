-- Smoke tests for syslogger (since GP syslogger code is dvergent from upstream)

create or replace function get_log_master(log_message text)
returns table (log_time timestamp WITH TIME ZONE,
               log_session text,
               log_msg text) as
$$
declare
  session_id text;
  curtime timestamp;
begin
  select now()::timestamp at TIME ZONE 'CST' as "Timestamp CST" into curtime;
  select current_setting('gp_session_id') into session_id;
  session_id = 'con' || session_id;
  raise log '%', log_message;
  return query select logtime, logsession, logmessage from gp_toolkit.gp_log_master_concise where logmessage = log_message and logtime > curtime and logsession = session_id;
end;
$$ language plpgsql;

select get_log_master('message with a " mark');
select get_log_master('message with two "" mark');
select get_log_master('message with three """ mark');
select get_log_master('message with foo"');
select get_log_master('message with ""foo""bar"');
select get_log_master('message with no quotes');
