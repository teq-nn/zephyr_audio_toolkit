# RPMsg Channel Contract Template

Use this template to define the message contract between cores.

## Channel Metadata
- Channel name:
- Local endpoint:
- Remote endpoint:
- Transport:

## Message Types
| Type ID | Direction | Payload Schema | Ack Required |
| --- | --- | --- | --- |
| 0x01 | host -> rt | | yes/no |
| 0x02 | rt -> host | | yes/no |

## Timing and Throughput
- Max expected message rate:
- Max payload size:
- Worst-case end-to-end latency target:

## Error Handling
- Timeout behavior:
- Retry policy:
- Backpressure policy:

## Versioning
- Protocol version field location:
- Backward compatibility strategy:
