#include <muduo/base/LogFile.h>

#include <muduo/base/FileUtil.h>
#include <muduo/base/ProcessInfo.h>

#include <assert.h>
#include <stdio.h>
#include <time.h>

using namespace muduo;

LogFile::LogFile(const string& basename,
                 size_t rollSize,
                 bool threadSafe,
                 int flushInterval,
                 int checkEveryN)
  : basename_(basename),
    rollSize_(rollSize),
    flushInterval_(flushInterval),
    checkEveryN_(checkEveryN),
    count_(0),
    mutex_(threadSafe ? new MutexLock : NULL),
    startOfPeriod_(0),
    lastRoll_(0),
    lastFlush_(0)
{
  assert(basename.find('/') == string::npos);
  rollFile();
}

LogFile::~LogFile()
{
}

void LogFile::append(const char* logline, int len)  //追加日志文本
{
  if (mutex_)   //如果new过锁了，说明需要线程安全，那么调用加锁方式
  {
    MutexLockGuard lock(*mutex_);
    append_unlocked(logline, len);
  }
  else
  {
    append_unlocked(logline, len);  //否则调用非加锁方式
  }
}

void LogFile::flush()
{
  if (mutex_)
  {
    MutexLockGuard lock(*mutex_);
    file_->flush();
  }
  else
  {
    file_->flush();
  }
}

void LogFile::append_unlocked(const char* logline, int len)
{
  file_->append(logline, len);  //调用成员变量file_的方法，也就是FileUtil.h中的AppendFile的不加锁apeend方法
  if (file_->writtenBytes() > rollSize_)   //写入字节数大小超过滚动设定大小
  {
    rollFile();  //滚动
  }
  else
  {
    ++count_;   //增加行数
    if (count_ >= checkEveryN_)
    {
      count_ = 0;
      time_t now = ::time(NULL);
      time_t thisPeriod_ = now / kRollPerSeconds_ * kRollPerSeconds_;
      if (thisPeriod_ != startOfPeriod_)  //比较事件是否相等，不等就是第二天0点，那么滚动
      {
        rollFile();
      }
      else if (now - lastFlush_ > flushInterval_)  //判断是否超过flush间隔时间，超过了就flush，否则什么都不做。
      {
        lastFlush_ = now; //更新
        file_->flush();
      }
    }
  }
}

bool LogFile::rollFile()
{
  time_t now = 0;
  string filename = getLogFileName(basename_, &now);  //获取生成一个文件名称

  //注意，这里先除KRollPerSeconds然后乘KPollPerSeconds表示对齐值KRollPerSeconds的整数倍，也就是事件调整到当天零点(/除法会引发取整)
  time_t start = now / kRollPerSeconds_ * kRollPerSeconds_;   

  if (now > lastRoll_)   //如果大于lastRoll，产生一个新的日志文件，并更新lastRoll
  {
    lastRoll_ = now;
    lastFlush_ = now;
    startOfPeriod_ = start;
    file_.reset(new FileUtil::AppendFile(filename));
    return true;
  }
  return false;
}

string LogFile::getLogFileName(const string& basename, time_t* now)
{
  string filename;
  filename.reserve(basename.size() + 64);  //预分配内存
  filename = basename;

  char timebuf[32];
  struct tm tm;
  *now = time(NULL);
  gmtime_r(now, &tm); // FIXME: localtime_r ?   //获取当前时间，UTC时间，_r是线程安全，
  strftime(timebuf, sizeof timebuf, ".%Y%m%d-%H%M%S.", &tm);  //格式化时间，strftime函数
  filename += timebuf;

  filename += ProcessInfo::hostname();  //根据processinfo模块内部调用::gethostname函数获取主机名

  char pidbuf[32];
  snprintf(pidbuf, sizeof pidbuf, ".%d", ProcessInfo::pid());  //进程号
  filename += pidbuf;

  filename += ".log"; 

  return filename;
}

